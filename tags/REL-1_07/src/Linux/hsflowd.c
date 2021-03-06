/* This software is distributed under the following license:
 * http://host-sflow.sourceforge.net/license.html
 */


#if defined(__cplusplus)
extern "C" {
#endif

#define HSFLOWD_MAIN

#include "hsflowd.h"

  // globals - easier for signal handler
  HSP HSPSamplingProbe;
  int exitStatus = EXIT_SUCCESS;
  extern int debug;

  /*_________________---------------------------__________________
    _________________     agent callbacks       __________________
    -----------------___________________________------------------
  */
  
  static void *agentCB_alloc(void *magic, SFLAgent *agent, size_t bytes)
  {
    return my_calloc(bytes);
  }

  static int agentCB_free(void *magic, SFLAgent *agent, void *obj)
  {
    free(obj);
    return 0;
  }

  static void agentCB_error(void *magic, SFLAgent *agent, char *msg)
  {
    myLog(LOG_ERR, "sflow agent error: %s", msg);
  }

  
  static void agentCB_sendPkt(void *magic, SFLAgent *agent, SFLReceiver *receiver, u_char *pkt, uint32_t pktLen)
  {
    HSP *sp = (HSP *)magic;
    size_t socklen = 0;
    int fd = 0;

    for(HSPCollector *coll = sp->sFlow->sFlowSettings->collectors; coll; coll=coll->nxt) {

      switch(coll->ipAddr.type) {
      case SFLADDRESSTYPE_UNDEFINED:
	// skip over it if the forward lookup failed
	break;
      case SFLADDRESSTYPE_IP_V4:
	{
	  struct sockaddr_in *sa = (struct sockaddr_in *)&(coll->sendSocketAddr);
	  socklen = sizeof(struct sockaddr_in);
	  sa->sin_family = AF_INET;
	  sa->sin_port = htons(coll->udpPort);
	  fd = sp->socket4;
	}
	break;
      case SFLADDRESSTYPE_IP_V6:
	{
	  struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&(coll->sendSocketAddr);
	  socklen = sizeof(struct sockaddr_in6);
	  sa6->sin6_family = AF_INET6;
	  sa6->sin6_port = htons(coll->udpPort);
	  fd = sp->socket6;
	}
	break;
      }

      if(socklen && fd > 0) {
	int result = sendto(fd,
			    pkt,
			    pktLen,
			    0,
			    (struct sockaddr *)&coll->sendSocketAddr,
			    socklen);
	if(result == -1 && errno != EINTR) {
	  myLog(LOG_ERR, "socket sendto error: %s", strerror(errno));
	}
	if(result == 0) {
	  myLog(LOG_ERR, "socket sendto returned 0: %s", strerror(errno));
	}
      }
    }
  }

#ifdef HSF_XEN
  static void openXenHandles(HSP *sp)
  {
    // need to do this while we still have root privileges
    if(sp->xc_handle == 0) {
      sp->xc_handle = xc_interface_open();
      if(sp->xc_handle <= 0) {
        myLog(LOG_ERR, "xc_interface_open() failed : %s", strerror(errno));
      }
      else {
        sp->xs_handle = xs_daemon_open_readonly();
        if(sp->xs_handle == NULL) {
          myLog(LOG_ERR, "xs_daemon_open_readonly() failed : %s", strerror(errno));
        }
        // get the page size [ref xenstat.c]
#if defined(PAGESIZE)
        sp->page_size = PAGESIZE;
#elif defined(PAGE_SIZE)
        sp->page_size = PAGE_SIZE;
#else
        sp->page_size = sysconf(_SC_PAGE_SIZE);
        if(pgsiz < 0) {
          myLog(LOG_ERR, "Failed to retrieve page size : %s", strerror(errno));
          abort();
        }
#endif
      }
    }
  }

  // if sp->xs_handle is not NULL then we know that sp->xc_handle is good too
  // because of the way we opened the handles in the first place.
  static int xenHandlesOK(HSP *sp) { return (sp->xs_handle != NULL); }

  static void closeXenHandles(HSP *sp)
  {
    if(sp->xc_handle && sp->xc_handle != -1) {
      xc_interface_close(sp->xc_handle);
      sp->xc_handle = 0;
    }
    if(sp->xs_handle) {
      xs_daemon_close(sp->xs_handle);
      sp->xs_handle = NULL;
    }
  }

  static int readVNodeCounters(HSP *sp, SFLHost_vrt_node_counters *vnode)
  {
    if(xenHandlesOK(sp)) {
      xc_physinfo_t physinfo = { 0 };
      if(xc_physinfo(sp->xc_handle, &physinfo) < 0) {
	myLog(LOG_ERR, "xc_physinfo() failed : %s", strerror(errno));
      }
      else {
      	vnode->mhz = (physinfo.cpu_khz / 1000);
	vnode->cpus = physinfo.nr_cpus;
	vnode->memory = ((uint64_t)physinfo.total_pages * sp->page_size);
	vnode->memory_free = ((uint64_t)physinfo.free_pages * sp->page_size);
	vnode->num_domains = sp->num_domains;
	return YES;
      }
    }
    return NO;
  }

  static SFLAdaptorList *xenstat_adaptors(HSP *sp, uint32_t dom_id, SFLAdaptorList *myAdaptors)
  {
    for(uint32_t i = 0; i < sp->adaptorList->num_adaptors; i++) {
      SFLAdaptor *adaptor = sp->adaptorList->adaptors[i];
      uint32_t vif_domid;
      uint32_t vif_netid;
      int isVirtual = (sscanf(adaptor->deviceName, "vif%"SCNu32".%"SCNu32, &vif_domid, &vif_netid) == 2);
      if((isVirtual && dom_id == vif_domid) ||
	 (!isVirtual && dom_id == 0)) {
	// include this one (if we have room)
	if(myAdaptors->num_adaptors < HSP_MAX_VIFS) {
	  myAdaptors->adaptors[myAdaptors->num_adaptors++] = adaptor;
	  if(isVirtual) {
	    // for virtual interfaces we need to query for the MAC address
	    char macQuery[256];
	    snprintf(macQuery, sizeof(macQuery), "/local/domain/%u/device/vif/%u/mac", vif_domid, vif_netid);
	    char *macStr = xs_read(sp->xs_handle, XBT_NULL, macQuery, NULL);
	    if(macStr == NULL) {
	      myLog(LOG_ERR, "mac address query failed : %s : %s", macQuery, strerror(errno));
	    }
	    else{
	      // got it - but make sure there is a place to write it
	      if(adaptor->num_macs > 0) {
		// OK, just overwrite the 'dummy' one that was there
		if(hexToBinary((u_char *)macStr, adaptor->macs[0].mac, 6) != 6) {
		  myLog(LOG_ERR, "mac address format error in xenstore query <%s> : %s", macQuery, macStr);
		}
	      }
	      free(macStr);
	    }
	  }
	}
      }
    }
    return myAdaptors;
  }

#endif

  void agentCB_getCounters(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
    assert(poller->magic);
    HSP *sp = (HSP *)poller->magic;

    // host ID
    SFLCounters_sample_element hidElem = { 0 };
    hidElem.tag = SFLCOUNTERS_HOST_HID;
    char hnamebuf[SFL_MAX_HOSTNAME_CHARS+1];
    char osrelbuf[SFL_MAX_OSRELEASE_CHARS+1];
    if(readHidCounters(sp,
		       &hidElem.counterBlock.host_hid,
		       hnamebuf,
		       SFL_MAX_HOSTNAME_CHARS,
		       osrelbuf,
		       SFL_MAX_OSRELEASE_CHARS)) {
      SFLADD_ELEMENT(cs, &hidElem);
    }

    // host Net I/O
    SFLCounters_sample_element nioElem = { 0 };
    nioElem.tag = SFLCOUNTERS_HOST_NIO;
    if(readNioCounters(sp, &nioElem.counterBlock.host_nio, NULL, NULL)) {
      SFLADD_ELEMENT(cs, &nioElem);
    }

    // host cpu counters
    SFLCounters_sample_element cpuElem = { 0 };
    cpuElem.tag = SFLCOUNTERS_HOST_CPU;
    if(readCpuCounters(&cpuElem.counterBlock.host_cpu)) {
      SFLADD_ELEMENT(cs, &cpuElem);
    }

    // host memory counters
    SFLCounters_sample_element memElem = { 0 };
    memElem.tag = SFLCOUNTERS_HOST_MEM;
    if(readMemoryCounters(&memElem.counterBlock.host_mem)) {
      SFLADD_ELEMENT(cs, &memElem);
    }

    // host I/O counters
    SFLCounters_sample_element dskElem = { 0 };
    dskElem.tag = SFLCOUNTERS_HOST_DSK;
    if(readDiskCounters(sp, &dskElem.counterBlock.host_dsk)) {
      SFLADD_ELEMENT(cs, &dskElem);
    }

    // include the adaptor list
    SFLCounters_sample_element adaptorsElem = { 0 };
    adaptorsElem.tag = SFLCOUNTERS_ADAPTORS;
    adaptorsElem.counterBlock.adaptors = sp->adaptorList;
    SFLADD_ELEMENT(cs, &adaptorsElem);

#ifdef HSF_XEN
    // replace the adaptorList with a filtered version of the same
      SFLAdaptorList myAdaptors;
      SFLAdaptor *adaptors[HSP_MAX_VIFS];
      myAdaptors.adaptors = adaptors;
      myAdaptors.capacity = HSP_MAX_VIFS;
      myAdaptors.num_adaptors = 0;
      adaptorsElem.counterBlock.adaptors = xenstat_adaptors(sp, 0, &myAdaptors);

    // hypervisor node stats
    SFLCounters_sample_element vnodeElem = { 0 };
    vnodeElem.tag = SFLCOUNTERS_HOST_VRT_NODE;
    if(readVNodeCounters(sp, &vnodeElem.counterBlock.host_vrt_node)) {
      SFLADD_ELEMENT(cs, &vnodeElem);
    }
#endif

    sfl_poller_writeCountersSample(poller, cs);
  }

#ifdef HSF_XEN

#define HSP_MAX_PATHLEN 256
#define XEN_SYSFS_VBD_PATH "/sys/devices/xen-backend"

  static int64_t xen_vbd_counter(char *vbd_type, uint32_t dom_id, uint32_t vbd_dev, char *counter)
  {
    int64_t ctr64 = 0;
    char path[HSP_MAX_PATHLEN];
    snprintf(path, HSP_MAX_PATHLEN, XEN_SYSFS_VBD_PATH "/%s-%u-%u/statistics/%s",
	     vbd_type,
	     dom_id,
	     vbd_dev,
	     counter);
    FILE *file = fopen(path, "r");
    if(file) {
      fscanf(file, "%"SCNi64, &ctr64);
      fclose(file);
    }
    return ctr64;
  }
  
  static int xenstat_dsk(HSP *sp, uint32_t dom_id, SFLHost_vrt_dsk_counters *dsk)
  {
    // [ref xenstat_linux.c]
#define SYSFS_VBD_PATH "/sys/devices/xen-backend/"
    DIR *sysfsvbd = opendir(SYSFS_VBD_PATH);
    if(sysfsvbd == NULL) {
      myLog(LOG_ERR, "opendir %s failed : %s", SYSFS_VBD_PATH, strerror(errno));
      return NO;
    }

    char scratch[sizeof(struct dirent) + _POSIX_PATH_MAX];
    struct dirent *dp = NULL;
    for(;;) {
      readdir_r(sysfsvbd, (struct dirent *)scratch, &dp);
      if(dp == NULL) break;
      uint32_t vbd_dom_id;
      uint32_t vbd_dev;
      char vbd_type[256];
      if(sscanf(dp->d_name, "%3s-%u-%u", vbd_type, &vbd_dom_id, &vbd_dev) == 3) {
	if(vbd_dom_id == dom_id) {
	  //dsk->capacity $$$
	  //dsk->allocation $$$
	  //dsk->available $$$
	  if(debug > 1) myLog(LOG_INFO, "reading VBD %s for dom_id %u", dp->d_name, dom_id); 
	  dsk->rd_req += xen_vbd_counter(vbd_type, dom_id, vbd_dev, "rd_req");
	  dsk->rd_bytes += (xen_vbd_counter(vbd_type, dom_id, vbd_dev, "rd_sect") * HSP_SECTOR_BYTES);
	  dsk->wr_req += xen_vbd_counter(vbd_type, dom_id, vbd_dev, "wr_req");
	  dsk->wr_bytes += (xen_vbd_counter(vbd_type, dom_id, vbd_dev, "wr_sect") * HSP_SECTOR_BYTES);
	  dsk->errs += xen_vbd_counter(vbd_type, dom_id, vbd_dev, "oo_req");
	}
      }
    }
    closedir(sysfsvbd);
    return YES;
  }
  
#endif

  void agentCB_getCountersVM(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
    assert(poller->magic);
    HSP *sp = (HSP *)poller->magic;
    HSPVMState *state = (HSPVMState *)poller->userData;
    if(state == NULL) return;

#ifdef HSF_XEN

    if(xenHandlesOK(sp)) {
      
      xc_domaininfo_t domaininfo;
      int32_t n = xc_domain_getinfolist(sp->xc_handle, state->vm_index, 1, &domaininfo);
      if(n < 0 || domaininfo.domain != state->domId) {
	// Assume something changed under our feet.
	// Request a reload of the VM information and bail.
	// We'll try again next time.
	myLog(LOG_INFO, "request for vm_index %u (dom_id=%u) returned %d (with dom_id=%u)",
	      state->vm_index,
	      state->domId,
	      n,
	      domaininfo.domain);
	sp->refreshVMList = YES;
	return;
      }
      
      // host ID
      SFLCounters_sample_element hidElem = { 0 };
      hidElem.tag = SFLCOUNTERS_HOST_HID;
      char query[255];
      char hname[255];
      snprintf(query, sizeof(query), "/local/domain/%u/name", state->domId);
      char *xshname = (char *)xs_read(sp->xs_handle, XBT_NULL, query, NULL);
      if(xshname) {
	// copy the name out here so we can free it straight away
	strncpy(hname, xshname, 255);
	free(xshname);
	hidElem.counterBlock.host_hid.hostname.str = hname;
	hidElem.counterBlock.host_hid.hostname.len = strlen(hname);
	memcpy(hidElem.counterBlock.host_hid.uuid, &domaininfo.handle, 16);
	hidElem.counterBlock.host_hid.machine_type = SFLMT_unknown;
	hidElem.counterBlock.host_hid.os_name = SFLOS_unknown;
	//hidElem.counterBlock.host_hid.os_release.str = NULL;
	//hidElem.counterBlock.host_hid.os_release.len = 0;
	SFLADD_ELEMENT(cs, &hidElem);
      }
      
      // host parent
      SFLCounters_sample_element parElem = { 0 };
      parElem.tag = SFLCOUNTERS_HOST_PAR;
      parElem.counterBlock.host_par.dsClass = SFL_DSCLASS_PHYSICAL_ENTITY;
      parElem.counterBlock.host_par.dsIndex = 1;
      SFLADD_ELEMENT(cs, &parElem);

      // VM Net I/O
      SFLCounters_sample_element nioElem = { 0 };
      nioElem.tag = SFLCOUNTERS_HOST_VRT_NIO;
      char devFilter[20];
      snprintf(devFilter, 20, "vif%u.", state->domId);
      uint32_t network_count = readNioCounters(sp, (SFLHost_nio_counters *)&nioElem.counterBlock.host_vrt_nio, devFilter, NULL);
      if(state->network_count != network_count) {
	// request a refresh if the number of VIFs changed. Not a perfect test
	// (e.g. if one was removed and another was added at the same time then
	// we would miss it). I guess we should keep the whole list of network ids,
	// or just force a refresh every few minutes?
	myLog(LOG_INFO, "vif count changed from %u to %u (dom_id=%u). Setting refreshAdaptorList=YES",
	      state->network_count,
	      network_count,
	      state->domId);
	state->network_count = network_count;
	sp->refreshAdaptorList = YES;
      }
      SFLADD_ELEMENT(cs, &nioElem);

      // VM cpu counters [ref xenstat.c]
      SFLCounters_sample_element cpuElem = { 0 };
      cpuElem.tag = SFLCOUNTERS_HOST_VRT_CPU;
      u_int64_t vcpu_ns = 0;
      for(uint32_t c = 0; c <= domaininfo.max_vcpu_id; c++) {
	xc_vcpuinfo_t info;
	if(xc_vcpu_getinfo(sp->xc_handle, state->domId, c, &info) != 0) {
	  // error or domain is in transition.  Just bail.
	  myLog(LOG_INFO, "vcpu list in transition (dom_id=%u)", state->domId);
	  return;
	}
	else {
	  if(info.online) {
	    vcpu_ns += info.cpu_time;
	  }
	}
      }
      uint32_t st = domaininfo.flags;
      // first 8 bits (b7-b0) are a mask of flags (see tools/libxc/xen/domctl.h)
      // next 8 bits (b15-b8) indentify the CPU to which the domain is bound
      // next 8 bits (b23-b16) indentify the the user-supplied shutdown code
      cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_NOSTATE;
      if(st & XEN_DOMINF_shutdown) {
	cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_SHUTDOWN;
	if(((st >> XEN_DOMINF_shutdownshift) & XEN_DOMINF_shutdownmask) == SHUTDOWN_crash) {
	  cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_CRASHED;
	}
      }
      else if(st & XEN_DOMINF_paused) cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_PAUSED;
      else if(st & XEN_DOMINF_blocked) cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_BLOCKED;
      else if(st & XEN_DOMINF_running) cpuElem.counterBlock.host_vrt_cpu.state = SFL_VIR_DOMAIN_RUNNING;
      // SFL_VIR_DOMAIN_SHUTOFF ?
      // other domaininfo flags include:
      // XEN_DOMINF_dying      : not sure when this is set -- perhaps always :)
      // XEN_DOMINF_hvm_guest  : as opposed to a PV guest
      // XEN_DOMINF_debugged   :

      cpuElem.counterBlock.host_vrt_cpu.cpuTime = (vcpu_ns / 1000000);
      cpuElem.counterBlock.host_vrt_cpu.nrVirtCpu = domaininfo.max_vcpu_id + 1;
      SFLADD_ELEMENT(cs, &cpuElem);

      // VM memory counters [ref xenstat.c]
      SFLCounters_sample_element memElem = { 0 };
      memElem.tag = SFLCOUNTERS_HOST_VRT_MEM;

      if(debug) myLog(LOG_INFO, "vm domid=%u, dsIndex=%u, vm_index=%u, tot_pages=%u",
		      state->domId,
		      SFL_DS_INDEX(poller->dsi),
		      state->vm_index,
		      domaininfo.tot_pages);

		      
      memElem.counterBlock.host_vrt_mem.memory = domaininfo.tot_pages * sp->page_size;
      memElem.counterBlock.host_vrt_mem.maxMemory = (domaininfo.max_pages == UINT_MAX) ? -1 : (domaininfo.max_pages * sp->page_size);
      SFLADD_ELEMENT(cs, &memElem);

      // VM disk I/O counters
      SFLCounters_sample_element dskElem = { 0 };
      dskElem.tag = SFLCOUNTERS_HOST_VRT_DSK;
      if(xenstat_dsk(sp, state->domId, &dskElem.counterBlock.host_vrt_dsk)) {
	SFLADD_ELEMENT(cs, &dskElem);
      }

      // include my slice of the adaptor list - and update
      // the MAC with the correct one at the same time
      SFLCounters_sample_element adaptorsElem = { 0 };
      adaptorsElem.tag = SFLCOUNTERS_ADAPTORS;
      SFLAdaptorList myAdaptors;
      SFLAdaptor *adaptors[HSP_MAX_VIFS];
      myAdaptors.adaptors = adaptors;
      myAdaptors.capacity = HSP_MAX_VIFS;
      myAdaptors.num_adaptors = 0;
      adaptorsElem.counterBlock.adaptors = xenstat_adaptors(sp, state->domId, &myAdaptors);
      SFLADD_ELEMENT(cs, &adaptorsElem);

      
      sfl_poller_writeCountersSample(poller, cs);
    }

#endif /* HSF_XEN */
#ifdef HSF_VRT
    virDomainPtr domainPtr = virDomainLookupByID(sp->virConn, state->domId);
    if(domainPtr == NULL) {
      sp->refreshVMList = YES;
    }
    else {
      // host ID
      SFLCounters_sample_element hidElem = { 0 };
      hidElem.tag = SFLCOUNTERS_HOST_HID;
      const char *hname = virDomainGetName(domainPtr); // no need to free this one
      if(hname) {
	// copy the name out here so we can free it straight away
	hidElem.counterBlock.host_hid.hostname.str = (char *)hname;
	hidElem.counterBlock.host_hid.hostname.len = strlen(hname);
	virDomainGetUUID(domainPtr, hidElem.counterBlock.host_hid.uuid);
	
	// char *osType = virDomainGetOSType(domainPtr); $$$
	hidElem.counterBlock.host_hid.machine_type = SFLMT_unknown;//$$$
	hidElem.counterBlock.host_hid.os_name = SFLOS_unknown;//$$$
	//hidElem.counterBlock.host_hid.os_release.str = NULL;
	//hidElem.counterBlock.host_hid.os_release.len = 0;
	SFLADD_ELEMENT(cs, &hidElem);
      }
      
      // host parent
      SFLCounters_sample_element parElem = { 0 };
      parElem.tag = SFLCOUNTERS_HOST_PAR;
      parElem.counterBlock.host_par.dsClass = SFL_DSCLASS_PHYSICAL_ENTITY;
      parElem.counterBlock.host_par.dsIndex = 1;
      SFLADD_ELEMENT(cs, &parElem);

      // VM Net I/O
      SFLCounters_sample_element nioElem = { 0 };
      nioElem.tag = SFLCOUNTERS_HOST_VRT_NIO;
      // since we are already maintaining the accumulated network counters (and handling issues like 32-bit
      // rollover) then we can just use the same mechanism again.  On a non-linux platform we may
      // want to take advantage of the libvirt call to get the counters (it takes the domain id and the
      // device name as parameters so you have to call it multiple times),  but even then we would
      // probably do that down inside the readNioCounters() fn in case there is work to do on the
      // accumulation and rollover-detection.
      readNioCounters(sp, (SFLHost_nio_counters *)&nioElem.counterBlock.host_vrt_nio, NULL, state->interfaces);
      SFLADD_ELEMENT(cs, &nioElem);
      
      // VM cpu counters [ref xenstat.c]
      SFLCounters_sample_element cpuElem = { 0 };
      cpuElem.tag = SFLCOUNTERS_HOST_VRT_CPU;
      virDomainInfo domainInfo;
      int domainInfoOK = NO;
      if(virDomainGetInfo(domainPtr, &domainInfo) != 0) {
	myLog(LOG_ERR, "virDomainGetInfo() failed");
      }
      else {
	domainInfoOK = YES;
	// enum virDomainState really is the same as enum SFLVirDomainState
	cpuElem.counterBlock.host_vrt_cpu.state = domainInfo.state;
	cpuElem.counterBlock.host_vrt_cpu.cpuTime = (domainInfo.cpuTime / 1000000);
	cpuElem.counterBlock.host_vrt_cpu.nrVirtCpu = domainInfo.nrVirtCpu;
	SFLADD_ELEMENT(cs, &cpuElem);
      }
      
      SFLCounters_sample_element memElem = { 0 };
      memElem.tag = SFLCOUNTERS_HOST_VRT_MEM;
      if(domainInfoOK) {
	memElem.counterBlock.host_vrt_mem.memory = domainInfo.memory * 1024;
	memElem.counterBlock.host_vrt_mem.maxMemory = (domainInfo.maxMem == UINT_MAX) ? -1 : (domainInfo.maxMem * 1024);
	SFLADD_ELEMENT(cs, &memElem);
      }

    
      // VM disk I/O counters
      SFLCounters_sample_element dskElem = { 0 };
      dskElem.tag = SFLCOUNTERS_HOST_VRT_DSK;
      for(int i = strArrayN(state->volumes); --i >= 0; ) {
	char *volPath = strArrayAt(state->volumes, i);
	virStorageVolPtr volPtr = virStorageVolLookupByPath(sp->virConn, volPath);
	if(volPath == NULL) {
	  myLog(LOG_ERR, "virStorageLookupByPath(%s) failed", volPath);
	}
	else {
	  virStorageVolInfo volInfo;
	  if(virStorageVolGetInfo(volPtr, &volInfo) != 0) {
	    myLog(LOG_ERR, "virStorageVolGetInfo(%s) failed", volPath);
	  }
	  else {
	    dskElem.counterBlock.host_vrt_dsk.capacity += volInfo.capacity;
	    dskElem.counterBlock.host_vrt_dsk.allocation += volInfo.allocation;
	    dskElem.counterBlock.host_vrt_dsk.available += (volInfo.capacity - volInfo.allocation);
	    // reads, writes and errors $$$ ?
	  }
	}
	SFLADD_ELEMENT(cs, &dskElem);
      }
      
      // include my slice of the adaptor list
      SFLCounters_sample_element adaptorsElem = { 0 };
      adaptorsElem.tag = SFLCOUNTERS_ADAPTORS;
      adaptorsElem.counterBlock.adaptors = state->interfaces;
      SFLADD_ELEMENT(cs, &adaptorsElem);
      
      
      sfl_poller_writeCountersSample(poller, cs);
      
      virDomainFree(domainPtr);
    }
#endif /* HSF_VRT */
  }

  /*_________________---------------------------__________________
    _________________    persistent dsIndex     __________________
    -----------------___________________________------------------
  */

  static HSPVMStore *newVMStore(HSP *sp, char *uuid, uint32_t dsIndex) {
    HSPVMStore *vmStore = (HSPVMStore *)my_calloc(sizeof(HSPVMStore));
    memcpy(vmStore->uuid, uuid, 16);
    vmStore->dsIndex = dsIndex;
    ADD_TO_LIST(sp->vmStore, vmStore);
    return vmStore;
  }

  static void readVMStore(HSP *sp) {
    if(sp->f_vmStore == NULL) return;
    char line[HSP_MAX_VMSTORE_LINELEN+1];
    rewind(sp->f_vmStore);
    uint32_t lineNo = 0;
    while(fgets(line, HSP_MAX_VMSTORE_LINELEN, sp->f_vmStore)) {
      lineNo++;
      char *p = line;
      // comments start with '#'
      p[strcspn(p, "#")] = '\0';
      // should just have two tokens, so check for 3
      uint32_t tokc = 0;
      char *tokv[3];
      for(int i = 0; i < 3; i++) {
	size_t len;
	p += strspn(p, HSP_VMSTORE_SEPARATORS);
	if((len = strcspn(p, HSP_VMSTORE_SEPARATORS)) == 0) break;
	tokv[tokc++] = p;
	p += len;
	if(*p != '\0') *p++ = '\0';
      }
      // expect UUID=int
      char uuid[16];
      if(tokc != 2 || !parseUUID(tokv[0], uuid)) {
	myLog(LOG_ERR, "readVMStore: bad line %u in %s", lineNo, sp->vmStoreFile);
      }
      else {
	HSPVMStore *vmStore = newVMStore(sp, uuid, strtol(tokv[1], NULL, 0));
	if(vmStore->dsIndex > sp->maxDsIndex) {
	  sp->maxDsIndex = vmStore->dsIndex;
	}
      }
    }
  }

  static void writeVMStore(HSP *sp) {
    rewind(sp->f_vmStore);
    for(HSPVMStore *vmStore = sp->vmStore; vmStore != NULL; vmStore = vmStore->nxt) {
      char uuidStr[51];
      printUUID((u_char *)vmStore->uuid, (u_char *)uuidStr, 50);
      fprintf(sp->f_vmStore, "%s=%u\n", uuidStr, vmStore->dsIndex);
    }
    fflush(sp->f_vmStore);
  }

  uint32_t assignVM_dsIndex(HSP *sp, char *uuid) {
    // check in case we saw this one before
    HSPVMStore *vmStore = sp->vmStore;
    for ( ; vmStore != NULL; vmStore = vmStore->nxt) {
      if(memcmp(uuid, vmStore->uuid, 16) == 0) return vmStore->dsIndex;
    }
    // allocate a new one
    vmStore = newVMStore(sp, uuid, ++sp->maxDsIndex);
    // ask it to be written to disk
    sp->vmStoreInvalid = YES;
    return sp->maxDsIndex;
  }


  /*_________________---------------------------__________________
    _________________    domain_xml_node        __________________
    -----------------___________________________------------------
  */

#ifdef HSF_VRT
  static char *get_xml_attr(xmlNode *node, char *attrName) {
    for(xmlAttr *attr = node->properties; attr; attr = attr->next) {
      if(attr->name) {
	if(debug) myLog(LOG_INFO, "attribute %s", attr->name);
	if(attr->children && !strcmp((char *)attr->name, attrName)) {
	  return (char *)attr->children->content;
	}
      }
    }
    return NULL;
  }
    
  void domain_xml_interface(xmlNode *node, HSPVMState *state, char **ifname, char **ifmac) {
    for(xmlNode *n = node; n; n = n->next) {
      if(n->type ==XML_ELEMENT_NODE) {
	if(n->name && n->parent && n->parent->name) {
	  if(!strcmp((char *)n->parent->name, "interface")) {
	    if(!strcmp((char *)n->name, "target")) {
	      char *dev = get_xml_attr(n, "dev");
	      if(dev) {
		if(debug) myLog(LOG_INFO, "interface.dev=%s", dev);
		if(ifname) *ifname = dev;
	      }
	    }
	    if(!strcmp((char *)n->name, "mac")) {
	      char *addr = get_xml_attr(n, "address");
	      if(debug) myLog(LOG_INFO, "interface.mac=%s", addr);
	      if(ifmac) *ifmac = addr;
	    }
	  }
	}
      }
      if(n->children) domain_xml_interface(n->children, state, ifname, ifmac);
    }
  }
    
  void domain_xml_node(xmlNode *node, HSPVMState *state) {
    for(xmlNode *n = node; n; n = n->next) {
      if(n->type == XML_ELEMENT_NODE && n->name &&
	 !strcmp((char *)n->name, "interface")) {
	char *ifname=NULL,*ifmac=NULL;
	domain_xml_interface(n, state, &ifname, &ifmac);
	if(*ifname && *ifmac) {
	  u_char macBytes[6];
	  if(hexToBinary((u_char *)ifmac, macBytes, 6) == 6) {
	    adaptorListAdd(state->interfaces, ifname, macBytes);
	  }
	}
      }
      else if(n->type == XML_ELEMENT_NODE && n->name && n->parent && n->parent->name &&
	      !strcmp((char *)n->parent->name, "disk") &&
	      !strcmp((char *)n->name, "source")) {
	char *path = get_xml_attr(n, "file");
	if(path) {
	  if(debug) myLog(LOG_INFO, "disk.path=%s", path);
	  strArrayAdd(state->volumes, (char *)path);
	}
      }
      else if(n->children) domain_xml_node(n->children, state);
    }
  }
#endif /* HSF_VRT */
  /*_________________---------------------------__________________
    _________________    configVMs              __________________
    -----------------___________________________------------------
  */
  
  static void configVMs(HSP *sp) {
    if(debug) myLog(LOG_INFO, "configVMs");
    HSPSFlow *sf = sp->sFlow;
    if(sf && sf->agent) {
      // mark and sweep
      // 1. mark all the current virtual pollers
      for(SFLPoller *pl = sf->agent->pollers; pl; pl = pl->nxt) {
	if(SFL_DS_CLASS(pl->dsi) == SFL_DSCLASS_LOGICAL_ENTITY) {
	  HSPVMState *state = (HSPVMState *)pl->userData;
	  state->marked = YES;
	}
      }

      // 2. create new VM pollers, or clear the mark on existing ones
#ifdef HSF_XEN
      
      if(xenHandlesOK(sp)) {
#define DOMAIN_CHUNK_SIZE 256
	xc_domaininfo_t domaininfo[DOMAIN_CHUNK_SIZE];
	int32_t num_domains=0, new_domains=0;
	do {
	  new_domains = xc_domain_getinfolist(sp->xc_handle,
					      num_domains,
					      DOMAIN_CHUNK_SIZE,
					      domaininfo);
	  if(new_domains < 0) {
	    myLog(LOG_ERR, "xc_domain_getinfolist() failed : %s", strerror(errno));
	  }
	  else {
	    for(uint32_t i = 0; i < new_domains; i++) {
	      uint32_t domId = domaininfo[i].domain;
	      // dom0 is the hypervisor. We want the others.
	      if(domId != 0) {
		if(debug) {
		  // may need to ignore any that are not marked as "running" here
		  myLog(LOG_INFO, "ConfigVMs(): domId=%u, flags=0x%x", domId, domaininfo[i].flags);
		}
		uint32_t dsIndex = assignVM_dsIndex(sp, (char *)&domaininfo[i].handle);
		SFLDataSource_instance dsi;
		// ds_class = <virtualEntity>, ds_index = <assigned>, ds_instance = 0
		SFL_DS_SET(dsi, SFL_DSCLASS_LOGICAL_ENTITY, dsIndex, 0);
		SFLPoller *vpoller = sfl_agent_addPoller(sf->agent, &dsi, sp, agentCB_getCountersVM);
		HSPVMState *state = (HSPVMState *)vpoller->userData;
		if(state) {
		  // it was already there, just clear the mark.
		  state->marked = NO;
		}
		else {
		  // new one - tell it what to do.
		  myLog(LOG_INFO, "configVMs: new domain=%u", domId);
		  uint32_t pollingInterval = sf->sFlowSettings ? sf->sFlowSettings->pollingInterval : SFL_DEFAULT_POLLING_INTERVAL;
		  sfl_poller_set_sFlowCpInterval(vpoller, pollingInterval);
		  sfl_poller_set_sFlowCpReceiver(vpoller, HSP_SFLOW_RECEIVER_INDEX);
		  // hang a new HSPVMState object on the userData hook
		  state = (HSPVMState *)my_calloc(sizeof(HSPVMState));
		  state->network_count = 0;
		  state->marked = NO;
		  vpoller->userData = state;
		  sp->refreshAdaptorList = YES;
		}
		// remember the index so we can access this individually later
		if(debug) {
		  if(state->vm_index != (num_domains + i)) {
		    myLog(LOG_INFO, "domId=%u vm_index %u->%u", domId, state->vm_index, (num_domains + i));
		  }
		}
		state->vm_index = num_domains + i;
		// and the domId, which might have changed (if vm rebooted)
		state->domId = domId;
	      }
	    }
	  }
	  num_domains += new_domains;
	} while(new_domains > 0);
	// remember the number of domains we found
	sp->num_domains = num_domains;
      }
#endif

#ifdef HSF_VRT
      int num_domains = virConnectNumOfDomains(sp->virConn);
      if(num_domains == -1) {
	myLog(LOG_ERR, "virConnectNumOfDomains() returned -1");
	return;
      }
      int *domainIds = (int *)my_calloc(num_domains * sizeof(int));
      if(virConnectListDomains(sp->virConn, domainIds, num_domains) != num_domains) {
	free(domainIds);
	return;
      }
      for(int i = 0; i < num_domains; i++) {
	int domId = domainIds[i];
	virDomainPtr domainPtr = virDomainLookupByID(sp->virConn, domId);
	if(domainPtr) {
	  char uuid[16];
	  virDomainGetUUID(domainPtr, (u_char *)uuid);
	  uint32_t dsIndex = assignVM_dsIndex(sp, uuid);
	  SFLDataSource_instance dsi;
	  // ds_class = <virtualEntity>, ds_index = <assigned>, ds_instance = 0
	  SFL_DS_SET(dsi, SFL_DSCLASS_LOGICAL_ENTITY, dsIndex, 0);
	  SFLPoller *vpoller = sfl_agent_addPoller(sf->agent, &dsi, sp, agentCB_getCountersVM);
	  HSPVMState *state = (HSPVMState *)vpoller->userData;
	  if(state) {
	    // it was already there, just clear the mark.
	    state->marked = NO;
	    // and reset the information that we are about to refresh
	    adaptorListReset(state->interfaces);
	    strArrayReset(state->volumes);
	  }
	  else {
	    // new one - tell it what to do.
	    myLog(LOG_INFO, "configVMs: new domain=%u", domId);
	    uint32_t pollingInterval = sf->sFlowSettings ? sf->sFlowSettings->pollingInterval : SFL_DEFAULT_POLLING_INTERVAL;
	    sfl_poller_set_sFlowCpInterval(vpoller, pollingInterval);
	    sfl_poller_set_sFlowCpReceiver(vpoller, HSP_SFLOW_RECEIVER_INDEX);
	    // hang a new HSPVMState object on the userData hook
	    state = (HSPVMState *)my_calloc(sizeof(HSPVMState));
	    state->network_count = 0;
	    state->marked = NO;
	    vpoller->userData = state;
	    state->interfaces = adaptorListNew();
	    state->volumes = strArrayNew();
	    sp->refreshAdaptorList = YES;
	  }
	  // remember the index so we can access this individually later
	  if(debug) {
	    if(state->vm_index != i) {
	      myLog(LOG_INFO, "domId=%u vm_index %u->%u", domId, state->vm_index, i);
	    }
	  }
	  state->vm_index = i;
	  // and the domId, which might have changed (if vm rebooted)
	  state->domId = domId;
	  
	  // get the XML descr - this seems more portable than some of
	  // the newer libvert API calls,  such as those to list interfaces
	  char *xmlstr = virDomainGetXMLDesc(domainPtr, 0 /*VIR_DOMAIN_XML_SECURE not allowed for read-only */);
	  if(xmlstr == NULL) {
	    myLog(LOG_ERR, "virDomainGetXMLDesc(domain=%u, 0) failed", domId);
	  }
	  else {
	    // parse the XML to get the list of interfaces and storage nodes
	    xmlDoc *doc = xmlParseMemory(xmlstr, strlen(xmlstr));
	    if(doc) {
	      xmlNode *rootNode = xmlDocGetRootElement(doc);
	      domain_xml_node(rootNode, state);
	      xmlFreeDoc(doc);
	    }
	    free(xmlstr);
	  }
	  xmlCleanupParser();

	  virDomainFree(domainPtr);
	}
      }
      // remember the number of domains we found
      sp->num_domains = num_domains;
      free(domainIds);
#endif
      
      // 3. remove any that don't exist any more
      for(SFLPoller *pl = sf->agent->pollers; pl; ) {
	SFLPoller *nextPl = pl->nxt;
	if(SFL_DS_CLASS(pl->dsi) == SFL_DSCLASS_LOGICAL_ENTITY) {
	  HSPVMState *state = (HSPVMState *)pl->userData;
	  if(state->marked) {
	    myLog(LOG_INFO, "configVMs: removing poller with dsIndex=%u (domId=%u)",
		  SFL_DS_INDEX(pl->dsi),
		  state->domId);
	    free(pl->userData);
	    pl->userData = NULL;
	    sfl_agent_removePoller(sf->agent, &pl->dsi);
	    sp->refreshAdaptorList = YES;
	  }
	}
	pl = nextPl;
      }
    }
  }
    
  /*_________________---------------------------__________________
    _________________       printIP             __________________
    -----------------___________________________------------------
  */
  
  static const char *printIP(SFLAddress *addr, char *buf, size_t len) {
    return inet_ntop(addr->type == SFLADDRESSTYPE_IP_V6 ? AF_INET6 : AF_INET,
		     &addr->address,
		     buf,
		     len);
  }

  /*_________________---------------------------__________________
    _________________    syncOutputFile         __________________
    -----------------___________________________------------------
  */
  
  static void syncOutputFile(HSP *sp) {
    if(debug) myLog(LOG_INFO, "syncOutputFile");
    rewind(sp->f_out);
    fprintf(sp->f_out, "# WARNING: Do not edit this file. It is generated automatically by hsflowd.\n");

    // revision appears both at the beginning and at the end
    fprintf(sp->f_out, "rev_start=%u\n", sp->sFlow->revisionNo);

    HSPSFlowSettings *settings = sp->sFlow->sFlowSettings;
    if(settings) {
      fprintf(sp->f_out, "sampling=%u\n", settings->samplingRate);
      fprintf(sp->f_out, "header=128\n");
      fprintf(sp->f_out, "polling=%u\n", settings->pollingInterval);
      char ipbuf[51];
      fprintf(sp->f_out, "agentIP=%s\n", printIP(&sp->sFlow->agentIP, ipbuf, 50));
      if(sp->sFlow->agentDevice) {
	fprintf(sp->f_out, "agent=%s\n", sp->sFlow->agentDevice);
      }
      for(HSPCollector *collector = settings->collectors; collector; collector = collector->nxt) {
	// <ip> <port> [<priority>]
	fprintf(sp->f_out, "collector=%s %u\n", printIP(&collector->ipAddr, ipbuf, 50), collector->udpPort);
      }
    }

    // repeat the revision number. The reader knows that if the revison number
    // has not changed under his feet then he has a consistent config.
    fprintf(sp->f_out, "rev_end=%u\n", sp->sFlow->revisionNo);
    fflush(sp->f_out);
  }

  /*_________________---------------------------__________________
    _________________       tick                __________________
    -----------------___________________________------------------
  */
  
  static void tick(HSP *sp) {
    
    // send a tick to the sFlow agent
    sfl_agent_tick(sp->sFlow->agent, sp->clk);
    
    // possibly poll the nio counters to avoid 32-bit rollover
    if(sp->adaptorNIOList.polling_secs &&
       ((sp->clk % sp->adaptorNIOList.polling_secs) == 0)) {
      updateNioCounters(sp);
    }
    
    // refresh the list of VMs if requested
    if(sp->refreshVMList || (sp->clk % HSP_REFRESH_VMS) == 0) {
      sp->refreshVMList = NO;
      configVMs(sp);
    }

    // write the persistent state if requested
    if(sp->vmStoreInvalid) {
      writeVMStore(sp);
      sp->vmStoreInvalid = NO;
    }

    // refresh the interface list if requested
    if(sp->refreshAdaptorList) {
      sp->refreshAdaptorList = NO;
      readInterfaces(sp);
    }


    // rewrite the output if the config has changed
    if(sp->outputRevisionNo != sp->sFlow->revisionNo) {
      syncOutputFile(sp);
      sp->outputRevisionNo = sp->sFlow->revisionNo;
    }
  }

  /*_________________---------------------------__________________
    _________________         initAgent         __________________
    -----------------___________________________------------------
  */
  
  static int initAgent(HSP *sp)
  {
    if(debug) myLog(LOG_INFO,"creating sfl agent");

    HSPSFlow *sf = sp->sFlow;
    
    if(sf->sFlowSettings == NULL) {
      myLog(LOG_ERR, "No sFlow config defined");
      return NO;
    }
    
    if(sf->sFlowSettings->collectors == NULL) {
      myLog(LOG_ERR, "No collectors defined");
      return NO;
    }

    assert(sf->agentIP.type);
    
    // open the sockets if not open already - one for v4 and another for v6
    if(sp->socket4 <= 0) {
      if((sp->socket4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	myLog(LOG_ERR, "IPv4 send socket open failed : %s", strerror(errno));
    }
    if(sp->socket6 <= 0) {
      if((sp->socket6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	myLog(LOG_ERR, "IPv6 send socket open failed : %s", strerror(errno));
    }

    time_t now = time(NULL);
    sf->agent = (SFLAgent *)my_calloc(sizeof(SFLAgent));
    sfl_agent_init(sf->agent,
		   &sf->agentIP,
		   sf->subAgentId,
		   now,
		   now,
		   sp,
		   agentCB_alloc,
		   agentCB_free,
		   agentCB_error,
		   agentCB_sendPkt);
    // just one receiver - we are serious about making this lightweight for now
    HSPCollector *collector = sf->sFlowSettings->collectors;
    SFLReceiver *receiver = sfl_agent_addReceiver(sf->agent);
    
    // claim the receiver slot
    sfl_receiver_set_sFlowRcvrOwner(receiver, "Virtual Switch sFlow Probe");
    
    // set the timeout to infinity
    sfl_receiver_set_sFlowRcvrTimeout(receiver, 0xFFFFFFFF);

    // receiver address/port - set it for the first collector,  but
    // actually we'll send the same feed to all collectors.  This step
    // may not be necessary at all when we are using the sendPkt callback.
    sfl_receiver_set_sFlowRcvrAddress(receiver, &collector->ipAddr);
    sfl_receiver_set_sFlowRcvrPort(receiver, collector->udpPort);
    
    uint32_t pollingInterval = sf->sFlowSettings ? sf->sFlowSettings->pollingInterval : SFL_DEFAULT_POLLING_INTERVAL;
    
    // add a <physicalEntity> poller to represent the whole physical host
    SFLDataSource_instance dsi;
  // ds_class = <physicalEntity>, ds_index = 1, ds_instance = 0
    SFL_DS_SET(dsi, SFL_DSCLASS_PHYSICAL_ENTITY, 1, 0);
    sf->poller = sfl_agent_addPoller(sf->agent, &dsi, sp, agentCB_getCounters);
    sfl_poller_set_sFlowCpInterval(sf->poller, pollingInterval);
    sfl_poller_set_sFlowCpReceiver(sf->poller, HSP_SFLOW_RECEIVER_INDEX);
    
    // add <virtualEntity> pollers for each virtual machine
    configVMs(sp);

    return YES;
  }

  /*_________________---------------------------__________________
    _________________     setDefaults           __________________
    -----------------___________________________------------------
  */

  static void setDefaults(HSP *sp)
  {
    sp->configFile = HSP_DEFAULT_CONFIGFILE;
    sp->outputFile = HSP_DEFAULT_OUTPUTFILE;
    sp->pidFile = HSP_DEFAULT_PIDFILE;
    sp->DNSSD_startDelay = HSP_DEFAULT_DNSSD_STARTDELAY;
    sp->DNSSD_retryDelay = HSP_DEFAULT_DNSSD_RETRYDELAY;
    sp->vmStoreFile = HSP_DEFAULT_VMSTORE_FILE;
    sp->dropPriv = YES;
  }

  /*_________________---------------------------__________________
    _________________      instructions         __________________
    -----------------___________________________------------------
  */

  static void instructions(char *command)
  {
    fprintf(stderr,"Usage: %s [-dvP] [-p PIDFile] [-u UUID] [-f CONFIGFile]\n", command);
    fprintf(stderr,"\n\
             -d:  debug mode - do not fork as a daemon, and log to stderr (repeat for more details)\n\
             -v:  print version number and exit\n\
             -P:  do not drop privileges (run as root)\n\
     -p PIDFile:  specify PID file (default is " HSP_DEFAULT_PIDFILE ")\n\
        -u UUID:  specify UUID as unique ID for this host\n\
  -f CONFIGFile:  specify config file (default is "HSP_DEFAULT_CONFIGFILE")\n\n");
  fprintf(stderr, "=============== More Information ============================================\n");
  fprintf(stderr, "| sFlow standard        - http://www.sflow.org                              |\n");
  fprintf(stderr, "| sFlowTrend (FREE)     - http://www.inmon.com/products/sFlowTrend.php      |\n");
  fprintf(stderr, "=============================================================================\n");

    exit(EXIT_FAILURE);
  }

  /*_________________---------------------------__________________
    _________________   processCommandLine      __________________
    -----------------___________________________------------------
  */

  static void processCommandLine(HSP *sp, int argc, char *argv[])
  {
    int in;
    while ((in = getopt(argc, argv, "dvPp:f:o:u:?h")) != -1) {
      switch(in) {
      case 'd': debug++; break;
      case 'v': printf("%s version %s\n", argv[0], HSP_VERSION); exit(EXIT_SUCCESS); break;
      case 'P': sp->dropPriv = NO; break;
      case 'p': sp->pidFile = optarg; break;
      case 'f': sp->configFile = optarg; break;
      case 'o': sp->outputFile = optarg; break;
      case 'u':
	if(parseUUID(optarg, sp->uuid) == NO) {
	  fprintf(stderr, "bad UUID format: %s\n", optarg);
	  instructions(*argv);
	}
	break;
      case '?':
      case 'h':
      default: instructions(*argv);
      }
    }
  }

  /*_________________---------------------------__________________
    _________________     setState              __________________
    -----------------___________________________------------------
  */

  static void setState(HSP *sp, EnumHSPState state) {
    if(debug) myLog(LOG_INFO, "state -> %s", HSPStateNames[state]);
    sp->state = state;
  }

  /*_________________---------------------------__________________
    _________________     signal_handler        __________________
    -----------------___________________________------------------
  */

  static void signal_handler(int sig) {
    HSP *sp = &HSPSamplingProbe;
    switch(sig) {
    case SIGTERM:
      myLog(LOG_INFO,"Received SIGTERM");
      setState(sp, HSPSTATE_END);
      break;
    case SIGINT:
      myLog(LOG_INFO,"Received SIGINT");
      setState(sp, HSPSTATE_END);
      break;
    default:
      myLog(LOG_INFO,"Received signal %d", sig);
      break;
    }
  }

  /*_________________---------------------------__________________
    _________________   installSFlowSettings    __________________
    -----------------___________________________------------------

    Always increment the revision number whenever we change the sFlowSettings pointer
  */
  
  static void installSFlowSettings(HSPSFlow *sf, HSPSFlowSettings *settings)
  {
    sf->sFlowSettings = settings;
    sf->revisionNo++;
  }

  /*_________________---------------------------__________________
    _________________        runDNSSD           __________________
    -----------------___________________________------------------
  */

  static void myDnsCB(HSP *sp, uint16_t rtype, uint32_t ttl, u_char *key, int keyLen, u_char *val, int valLen)
  {
    HSPSFlowSettings *st = sp->sFlow->sFlowSettings_dnsSD;

    // latch the min ttl
    if(sp->DNSSD_ttl == 0 || ttl < sp->DNSSD_ttl) {
      sp->DNSSD_ttl = ttl;
    }

    char keyBuf[1024];
    char valBuf[1024];
    if(keyLen > 1023 || valLen > 1023) {
      myLog(LOG_ERR, "myDNSCB: string too long");
      return;
    }
    // null terminate
    memcpy(keyBuf, (char *)key, keyLen);
    keyBuf[keyLen] = '\0';
    memcpy(valBuf, (char *)val, valLen);
    valBuf[valLen] = '\0';

    if(debug) {
      myLog(LOG_INFO, "dnsSD: (rtype=%u,ttl=%u) <%s>=<%s>", rtype, ttl, keyBuf, valBuf);
    }

    if(key == NULL && val && valLen > 3) {
      uint32_t delim = strcspn(valBuf, "/");
      if(delim > 0 && delim < valLen) {
	valBuf[delim] = '\0';
	HSPCollector *coll = newCollector(st);
	if(lookupAddress(valBuf, (struct sockaddr *)&coll->sendSocketAddr,  &coll->ipAddr, 0) == NO) {
	  myLog(LOG_ERR, "myDNSCB: SRV record returned hostname, but forward lookup failed");
	  // turn off the collector by clearing the address type
	  coll->ipAddr.type = SFLADDRESSTYPE_UNDEFINED;
	}
	coll->udpPort = strtol(valBuf + delim + 1, NULL, 0);
	if(coll->udpPort < 1 || coll->udpPort > 65535) {
	  myLog(LOG_ERR, "myDNSCB: SRV record returned hostname, but bad port: %d", coll->udpPort);
	  // turn off the collector by clearing the address type
	  coll->ipAddr.type = SFLADDRESSTYPE_UNDEFINED;
	}
      }
    }
    else if(strcmp(keyBuf, "sampling") == 0) {
      st->samplingRate = strtol(valBuf, NULL, 0);
    }
    else if(strcmp(keyBuf, "txtvers") == 0) {
    }
    else if(strcmp(keyBuf, "polling") == 0) {
      st->pollingInterval = strtol(valBuf, NULL, 0);
    }
    else {
      myLog(LOG_INFO, "unexpected dnsSD record <%s>=<%s>", keyBuf, valBuf);
    }
  }

  static void *runDNSSD(void *magic) {
    HSP *sp = (HSP *)magic;
    sp->DNSSD_countdown = sfl_random(sp->DNSSD_startDelay);
    time_t clk = time(NULL);
    while(1) {
      my_usleep(1000000);
      time_t test_clk = time(NULL);
      if((test_clk < clk) || (test_clk - clk) > HSP_MAX_TICKS) {
	// avoid a flurry of ticks if the clock jumps
	myLog(LOG_INFO, "time jump detected (DNSSD) %ld->%ld", clk, test_clk);
	clk = test_clk - 1;
      }
      time_t ticks = test_clk - clk;
      clk = test_clk;
      if(sp->DNSSD_countdown > ticks) {
	sp->DNSSD_countdown -= ticks;
      }
      else {
	// initiate server-discovery
	HSPSFlow *sf = sp->sFlow;
	sf->sFlowSettings_dnsSD = newSFlowSettings();
	// pick up default polling interval from config file
	sf->sFlowSettings_dnsSD->pollingInterval = sf->sFlowSettings_file->pollingInterval;
	// we want the min ttl, so clear it here
	sp->DNSSD_ttl = 0;
	// now make the requests
	int num_servers = dnsSD(sp, myDnsCB);
	SEMLOCK_DO(sp->config_mut) {
	  // three cases here:
	  // A) if(num_servers == -1) (i.e. query failed) then keep the current config
	  // B) if(num_servers == 0) then stop monitoring
	  // C) if(num_servers > 0) then install the new config
	  if(debug) myLog(LOG_INFO, "num_servers == %d", num_servers);
	  if(num_servers >= 0) {
	    // remove the current config
	    if(sf->sFlowSettings && sf->sFlowSettings != sf->sFlowSettings_file) freeSFlowSettings(sf->sFlowSettings);
	    installSFlowSettings(sf, NULL);
	  }
	  if(num_servers <= 0) {
	    // clean up, and go into 'retry' mode
	    freeSFlowSettings(sf->sFlowSettings_dnsSD);
	    sf->sFlowSettings_dnsSD = NULL;
	    // we might still learn a TTL (e.g. from the TXT record query)
	    sp->DNSSD_countdown = sp->DNSSD_ttl == 0 ? sp->DNSSD_retryDelay : sp->DNSSD_ttl;
	  }
	  else {
	    // make this the running config
	    installSFlowSettings(sf, sf->sFlowSettings_dnsSD);
	    sp->DNSSD_countdown = sp->DNSSD_ttl;
	  }
	  if(sp->DNSSD_countdown < HSP_DEFAULT_DNSSD_MINDELAY) {
	    if(debug) myLog(LOG_INFO, "forcing minimum DNS polling delay");
	    sp->DNSSD_countdown = HSP_DEFAULT_DNSSD_MINDELAY;
	  }
	  if(debug) myLog(LOG_INFO, "DNSSD polling delay set to %u seconds", sp->DNSSD_countdown);
	}
      }    
    }  
    return NULL;
  }
      
  /*_________________---------------------------__________________
    _________________         drop_privileges   __________________
    -----------------___________________________------------------
  */

  static int getMyLimit(int resource, char *resourceName) {
    struct rlimit rlim = {0};
    if(getrlimit(resource, &rlim) != 0) {
      myLog(LOG_ERR, "getrlimit(%s) failed : %s", resourceName, strerror(errno));
    }
    else {
      myLog(LOG_INFO, "getrlimit(%s) = %u (max=%u)", resourceName, rlim.rlim_cur, rlim.rlim_max);
    }
    return rlim.rlim_cur;
  }
  
  static int setMyLimit(int resource, char *resourceName, int request) {
    struct rlimit rlim = {0};
    rlim.rlim_cur = rlim.rlim_max = request;
    if(setrlimit(resource, &rlim) != 0) {
      myLog(LOG_ERR, "setrlimit(%s)=%d failed : %s", resourceName, request, strerror(errno));
      return NO;
    }
    else if(debug) {
      myLog(LOG_INFO, "setrlimit(%s)=%u", resourceName, request);
    }
    return YES;
  }
  
#define GETMYLIMIT(L) getMyLimit((L), STRINGIFY(L))
#define SETMYLIMIT(L,V) setMyLimit((L), STRINGIFY(L), (V))
  

  static void drop_privileges(int requestMemLockBytes) {
    
    if(getuid() != 0) return;
    
    if(requestMemLockBytes) {
      // Request to lock this process in memory so that we don't get
      // swapped out. It's probably less than 100KB,  and this way
      // we don't consume extra resources swapping in and out
      // every 20 seconds.  The default limit is just 32K on most
      // systems,  so for this to be useful we have to increase it
      // somewhat first.
#ifdef RLIMIT_MEMLOCK
      SETMYLIMIT(RLIMIT_MEMLOCK, requestMemLockBytes);
#endif
      // Because we are dropping privileges we can get away with
      // using the MLC_FUTURE option to mlockall without fear.  We
      // won't be allowed to lock more than the limit we just set
      // above.
      if(mlockall(MCL_FUTURE) == -1) {
	myLog(LOG_ERR, "mlockall(MCL_FUTURE) failed : %s", strerror(errno));
      }
      
      // We can also use this as an upper limit on the data segment so that we fail
      // if there is a memory leak,  rather than grow forever and cause problems.
#ifdef RLIMIT_DATA
      SETMYLIMIT(RLIMIT_DATA, requestMemLockBytes);
#endif
      
      // set the real and effective group-id to 'nobody'
      struct passwd *nobody = getpwnam("nobody");
      if(nobody == NULL) {
	myLog(LOG_ERR, "drop_privileges: user 'nobody' not found");
	exit(EXIT_FAILURE);
      }
      if(setgid(nobody->pw_gid) != 0) {
	myLog(LOG_ERR, "drop_privileges: setgid(%d) failed : %s", nobody->pw_gid, strerror(errno));
	exit(EXIT_FAILURE);
      }
      
      // It doesn't seem like this part is necessary(?)
      // if(initgroups("nobody", nobody->pw_gid) != 0) {
      //  myLog(LOG_ERR, "drop_privileges: initgroups failed : %s", strerror(errno));
      //  exit(EXIT_FAILURE);
      // }
      // endpwent();
      // endgrent();
      
      // now change user
      if(setuid(nobody->pw_uid) != 0) {
	myLog(LOG_ERR, "drop_privileges: setuid(%d) failed : %s", nobody->pw_uid, strerror(errno));
	exit(EXIT_FAILURE);
      }
      
      if(debug) {
	GETMYLIMIT(RLIMIT_MEMLOCK);
	GETMYLIMIT(RLIMIT_NPROC);
	GETMYLIMIT(RLIMIT_STACK);
	GETMYLIMIT(RLIMIT_CORE);
	GETMYLIMIT(RLIMIT_CPU);
	GETMYLIMIT(RLIMIT_DATA);
	GETMYLIMIT(RLIMIT_FSIZE);
	GETMYLIMIT(RLIMIT_RSS);
	GETMYLIMIT(RLIMIT_NOFILE);
	GETMYLIMIT(RLIMIT_AS);
	GETMYLIMIT(RLIMIT_LOCKS);
      }
    }
  }
  
  /*_________________---------------------------__________________
    _________________         main              __________________
    -----------------___________________________------------------
  */
  
  int main(int argc, char *argv[])
  {
    HSP *sp = &HSPSamplingProbe;

    // open syslog
    openlog(HSP_DAEMON_NAME, LOG_CONS, LOG_USER);
    setlogmask(LOG_UPTO(LOG_DEBUG));

    // register signal handler
    signal(SIGTERM,signal_handler);
    signal(SIGINT,signal_handler); 

    // init
    setDefaults(sp);

    // read the command line
    processCommandLine(sp, argc, argv);
      
    // don't run if we think another one is already running
    struct stat statBuf;
    if(stat(sp->pidFile, &statBuf) == 0) {
      myLog(LOG_ERR,"Another %s is already running. If this is an error, remove %s", argv[0], sp->pidFile);
      exit(EXIT_FAILURE);
    }

    if(debug == 0) {
      // fork to daemonize
      pid_t pid = fork();
      if(pid < 0) {
	myLog(LOG_ERR,"Cannot fork child");
	exit(EXIT_FAILURE);
      }
      
      if(pid > 0) {
	// in parent - write pid file and exit
	FILE *f;
	if(!(f = fopen(sp->pidFile,"w"))) {
	  myLog(LOG_ERR,"Could not open the pid file %s for writing : %s", sp->pidFile, strerror(errno));
	  exit(EXIT_FAILURE);
	}
	fprintf(f,"%"PRIu64"\n",(uint64_t)pid);
	if(fclose(f) == -1) {
	  myLog(LOG_ERR,"Could not close pid file %s : %s", sp->pidFile, strerror(errno));
	  exit(EXIT_FAILURE);
	}
	
	exit(EXIT_SUCCESS);
      }
      else {
	// in child

	// make sure the output file we write cannot then be written by some other non-root user
	umask(S_IWGRP | S_IWOTH);

	// new session - with me as process group leader
	pid_t sid = setsid();
	if(sid < 0) {
	  myLog(LOG_ERR,"setsid failed");
	  exit(EXIT_FAILURE);
	}
	
	// close all file descriptors 
	int i;
	for(i=getdtablesize(); i >= 0; --i) close(i);
	// create stdin/out/err
	i = open("/dev/null",O_RDWR); // stdin
	dup(i);                       // stdout
	dup(i);                       // stderr
      }
    }

    // open the output file while we still have root priviliges.
    // use mode "w+" because we intend to write it and rewrite it.
    if((sp->f_out = fopen(sp->outputFile, "w+")) == NULL) {
      myLog(LOG_ERR, "cannot open output file %s : %s", sp->outputFile);
      exit(EXIT_FAILURE);
    }
    
#ifdef HSF_XEN
    // open Xen handles while we still have root privileges
    openXenHandles(sp);
#endif

#ifdef HSF_VRT
    // open the libvirt connection
    int virErr = virInitialize();
    if(virErr != 0) {
      myLog(LOG_ERR, "virInitialize() failed: %d\n", virErr);
      exit(EXIT_FAILURE);
    }
    sp->virConn = virConnectOpenReadOnly(NULL);
    if(sp->virConn == NULL) {
      myLog(LOG_ERR, "virConnectOpenReadOnly() failed\n");
      exit(EXIT_FAILURE);
    }
#endif
    
#if defined(HSF_XEN) || defined(HSF_VRT)
    // open the vmStore file while we still have root priviliges
    // use mode "w+" because we intend to write it and rewrite it.
    if((sp->f_vmStore = fopen(sp->vmStoreFile, "w+")) == NULL) {
      myLog(LOG_ERR, "cannot open vmStore file %s : %s", sp->vmStoreFile);
      exit(EXIT_FAILURE);
    }
#endif

    if(sp->dropPriv) {
      // don't need to be root any more - we held on to root privileges
      // to make sure we could write the pid file,  and open the output
      // file, and open the Xen handles, and on Debian we needed to fork
      // the DNSSD thread before calling setuid (not sure why?).
      // Anway, from now on we don't want the responsibility...
      drop_privileges(HSP_RLIMIT_MEMLOCK);
    }

    myLog(LOG_INFO, "started");
    
    // initialize the clock so we can detect second boundaries
    sp->clk = time(NULL);

    // semaphore to protect config shared with DNSSD thread
    sp->config_mut = (pthread_mutex_t *)my_calloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(sp->config_mut, NULL);
    
    setState(sp, HSPSTATE_READCONFIG);

    while(sp->state != HSPSTATE_END) {
      
      switch(sp->state) {
	
      case HSPSTATE_READCONFIG:
	if(readInterfaces(sp) == 0 || HSPReadConfigFile(sp) == NO) {
	  exitStatus = EXIT_FAILURE;
	  setState(sp, HSPSTATE_END);
	}
	else {
	  // we must have an agentIP, so we can use
	  // it to seed the random number generator
	  SFLAddress *agentIP = &sp->sFlow->agentIP;
	  uint32_t seed;
	  if(agentIP->type == SFLADDRESSTYPE_IP_V4) seed = agentIP->address.ip_v4.addr;
	  else memcpy(agentIP->address.ip_v6.addr + 12, &seed, 4);
	  sfl_random_init(seed);

	
	  // load the persistent state from last time
	  readVMStore(sp);

	  // initialize the faster polling of NIO counters
	  // to avoid undetected 32-bit wraps
	  sp->adaptorNIOList.polling_secs = HSP_NIO_POLLING_SECS_32BIT;
	  
	  if(sp->DNSSD) {
	    // launch dnsSD thread.  It will now be responsible for
	    // the sFlowSettings,  and the current thread will loop
	    // in the HSPSTATE_WAITCONFIG state until that pointer
	    // has been set (sp->sFlow.sFlowSettings)
	    // Set a more conservative stacksize here - partly because
	    // we don't need more,  but mostly because Debian was refusing
	    // to create the thread - I guess because it was enough to
	    // blow through our mlockall() allocation.
	    // http://www.mail-archive.com/xenomai-help@gna.org/msg06439.html 
	    pthread_attr_t attr;
	    pthread_attr_init(&attr);
	    pthread_attr_setstacksize(&attr, HSP_DNSSD_STACKSIZE);
	    sp->DNSSD_thread = my_calloc(sizeof(pthread_t));
	    int err = pthread_create(sp->DNSSD_thread, &attr, runDNSSD, sp);
	    if(err != 0) {
	      myLog(LOG_ERR, "pthread_create() failed: %s\n", strerror(err));
	      exit(EXIT_FAILURE);
	    }
	  }
	  else {
	    // just use the config from the file
	    installSFlowSettings(sp->sFlow, sp->sFlow->sFlowSettings_file);
	  }
	  setState(sp, HSPSTATE_WAITCONFIG);
	}
	break;
	
      case HSPSTATE_WAITCONFIG:
	SEMLOCK_DO(sp->config_mut) {
	  if(sp->sFlow->sFlowSettings) {
	    // we have a config - proceed
	    if(initAgent(sp)) {
	      if(debug) {
		myLog(LOG_INFO, "initAgent suceeded");
		// print some stats to help us size HSP_RLIMIT_MEMLOCK etc.
		malloc_stats();
	      }
	      setState(sp, HSPSTATE_RUN);
	    }
	    else {
	      exitStatus = EXIT_FAILURE;
	      setState(sp, HSPSTATE_END);
	    }
	  }
	}
	break;
	
      case HSPSTATE_RUN:
	{
	  // check for second boundaries and generate ticks for the sFlow library
	  time_t test_clk = time(NULL);
	  if((test_clk < sp->clk) || (test_clk - sp->clk) > HSP_MAX_TICKS) {
	    // avoid a busy-loop of ticks
	    myLog(LOG_INFO, "time jump detected");
	    sp->clk = test_clk - 1;
	  }
	  while(sp->clk < test_clk) {

	    // this would be a good place to test the memory footprint and
	    // bail out if it looks like we are leaking memory(?)

	    SEMLOCK_DO(sp->config_mut) {
	      // was the config turned off?
	      if(sp->sFlow->sFlowSettings) {
		// did the polling interval change?  We have the semaphore
		// here so we can just run along and tell everyone.
		uint32_t piv = sp->sFlow->sFlowSettings->pollingInterval;
		if(piv != sp->previousPollingInterval) {
		  
		  if(debug) myLog(LOG_INFO, "polling interval changed from %u to %u",
				  sp->previousPollingInterval, piv);
		  
		  for(SFLPoller *pl = sp->sFlow->agent->pollers; pl; pl = pl->nxt) {
		    sfl_poller_set_sFlowCpInterval(pl, piv);
		  }
		  sp->previousPollingInterval = piv;
		}
		// clock-tick
		tick(sp);
	      }
	    } // semaphore
	    sp->clk++;
	  }
	}
      case HSPSTATE_END:
	break;
      }
      
      // set the timeout so that if all is quiet we will
      // still loop around and check for ticks/signals
      // several times per second
      my_usleep(200000);
    }

    // get here if a signal kicks the state to HSPSTATE_END
    // and we break out of the loop above.
    // If that doesn't happen the most likely explanation
    // is a bug that caused the semaphore to be acquired
    // and not released,  but that would only happen if the
    // DNSSD thread died or hung up inside the critical block.
    closelog();
    myLog(LOG_INFO,"stopped");
    
#ifdef HSF_XEN
    closeXenHandles(sp);
#endif

#ifdef HSF_VRT
    virConnectClose(sp->virConn);
#endif

    if(debug == 0) {
      // shouldn't need to be root again to remove the pidFile
      // (i.e. we should still have execute permission on /var/run)
      remove(sp->pidFile);
    }

    exit(exitStatus);
  } /* main() */


#if defined(__cplusplus)
} /* extern "C" */
#endif

