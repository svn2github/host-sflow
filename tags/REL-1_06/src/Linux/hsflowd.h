/* This software is distributed under the following license:
 * http://host-sflow.sourceforge.net/license.html
 */

#ifndef HSFLOWD_H
#define HSFLOWD_H 1

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <ctype.h>
#include <pthread.h>

#include <sys/types.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h> // for PRIu64 etc.
#include "sys/mman.h" // for mlockall()
#include "pwd.h" // for getpwnam()
#include "grp.h"
#include "sys/resource.h" // for setrlimit()
#include "malloc.h" // for malloc_stats()
#include "sflow_api.h"

#ifdef HSF_XEN
#include "xs.h"
#include "xenctrl.h"
#include "dirent.h"
#endif

#define YES 1
#define NO 0

#define ADD_TO_LIST(linkedlist, obj) \
  do { \
    obj->nxt = linkedlist; \
    linkedlist = obj; \
  } while(0)

#define HSP_VERSION "1.0.0"
#define HSP_DAEMON_NAME "hsflowd"
#define HSP_DEFAULT_PIDFILE "/var/run/hsflowd.pid"
#define HSP_DEFAULT_CONFIGFILE "/etc/hsflowd.conf"
#define HSP_DEFAULT_OUTPUTFILE "/etc/hsflowd.auto"
#define HSP_DEFAULT_VMSTORE_FILE "/etc/hsflowd.data"
#define HSP_DEFAULT_SUBAGENTID 100
#define HSP_MAX_TICKS 60
#define HSP_DEFAULT_DNSSD_STARTDELAY 30
#define HSP_DEFAULT_DNSSD_RETRYDELAY 300
#define HSP_DEFAULT_DNSSD_MINDELAY 10
#define HSP_DNSSD_STACKSIZE 2000000
#define HSP_MAX_SUBAGENTID 1000000
#define HSP_REFRESH_VMS 60

// the limit we will request before calling mlockall()
// calling res_search() seems to allocate about 11MB
// (not sure why), so set the limit accordingly.
#define HSP_RLIMIT_MEMLOCK (1024 * 1024 * 15)

  // only one receiver, so the receiverIndex is a constant
#define HSP_SFLOW_RECEIVER_INDEX 1

// just assume the sector size is 512 bytes
#define HSP_SECTOR_BYTES 512

// upper limit on number of VIFs per VM
#define HSP_MAX_VIFS 64

  // forward declarations
  struct _HSPSFlow;
  struct _HSP;

  typedef struct _HSPCollector {
    struct _HSPCollector *nxt;
    SFLAddress ipAddr;
    uint32_t udpPort;
    struct sockaddr_in6 sendSocketAddr;
  } HSPCollector;

  typedef struct _HSPSFlowSettings {
    HSPCollector *collectors;
    uint32_t numCollectors;
    uint32_t samplingRate;
    uint32_t pollingInterval;
  } HSPSFlowSettings;

  typedef struct _HSPSFlow {
    struct _HSP *myHSP;
    SFLAgent *agent;
    SFLPoller *poller;

    HSPSFlowSettings *sFlowSettings_file;
    HSPSFlowSettings *sFlowSettings_dnsSD;
    HSPSFlowSettings *sFlowSettings;
    uint32_t revisionNo;

    uint32_t subAgentId;
    SFLAdaptor *agentDevice;
    SFLAddress agentIP;
  } HSPSFlow; 

  typedef enum { HSPSTATE_READCONFIG=0,
		 HSPSTATE_WAITCONFIG,
		 HSPSTATE_RUN,
		 HSPSTATE_END
  } EnumHSPState;

#ifdef HSFLOWD_MAIN
  static const char *HSPStateNames[] = {
    "READCONFIG",
    "WAITCONFIG",
    "RUN",
    "END"
  };
#endif

  // persistent state for mapping VM domIds to
  // sFlow datasource indices
#define HSP_MAX_VMSTORE_LINELEN 100
#define HSP_VMSTORE_SEPARATORS " \t\r\n="
  typedef struct _HSPVMStore {
    struct _HSPVMStore *nxt;
    char uuid[16];
    uint32_t dsIndex;
  } HSPVMStore;
  

  // userData structure to store state for VM data-sources
  typedef struct _HSPVMState {
    uint32_t network_count;
    int32_t marked;
    uint32_t vm_index;
    uint32_t domId;
  } HSPVMState;

  // cache nio counters per adaptor
  typedef struct _HSPAdaptorNIO {
    char *deviceName;
    SFLHost_nio_counters nio;
    SFLHost_nio_counters last_nio;
    uint32_t last_bytes_in32;
    uint32_t last_bytes_out32;
#define HSP_MAX_NIO_DELTA32 0x7FFFFFFF
#define HSP_MAX_NIO_DELTA64 (uint64_t)(1.0e13)
    time_t last_update;
  } HSPAdaptorNIO;

  typedef struct _HSPAdaptorNIOList {
    HSPAdaptorNIO **adaptors;
    uint32_t num_adaptors;
    time_t last_update;
    // have to poll the NIO counters fast enough to avoid 32-bit rollover
    // of the bytes counters.  On a 10Gbps interface they can wrap in 
    // less than 5 seconds.  On a virtual interface the data rate could be
    // higher still. The program may decide to turn this off. For example,
    // if it finds evidence that the counters are already 64-bit in the OS,
    // or if it decides that all interface speeds are limited to 1Gbps or less.
    time_t polling_secs;
#define HSP_NIO_POLLING_SECS_32BIT 3
  } HSPAdaptorNIOList;

  typedef struct _HSPDiskIO {
    uint64_t last_sectors_read;
    uint64_t last_sectors_written;
    uint64_t bytes_read;
    uint64_t bytes_written;
  } HSPDiskIO;

  typedef struct _HSP {
    EnumHSPState state;
    time_t clk;
    HSPSFlow *sFlow;
    char *configFile;
    char *outputFile;
    char *pidFile;
    int dropPriv;
    uint32_t outputRevisionNo;
    FILE *f_out;
    // Identity
    char uuid[16];
    // interfaces and MACs
    SFLAdaptorList *adaptorList;
    HSPAdaptorNIOList adaptorNIOList;
    int refreshAdaptorList;
    int refreshVMList;
    // 64-bit diskIO accumulators
    HSPDiskIO diskIO;
    // UDP send sockets
    int socket4;
    int socket6;
#ifdef HSF_XEN
    int xc_handle; // libxc
    struct xs_handle *xs_handle; // xenstore
    uint32_t page_size;
    uint32_t num_domains;
#endif
    // persistent state
    uint32_t maxDsIndex;
    char *vmStoreFile;
    FILE *f_vmStore;
    int vmStoreInvalid;
    HSPVMStore *vmStore;
    // inter-thread communication
    pthread_mutex_t *config_mut;
    int DNSSD;
    char *DNSSD_domain;
    uint32_t previousPollingInterval;
    // the DNSSD thread and his private state
    pthread_t *DNSSD_thread;
    int DNSSD_countdown;
    uint32_t DNSSD_startDelay;
    uint32_t DNSSD_retryDelay;
    uint32_t DNSSD_ttl;
  } HSP;

  // expose some config parser fns
  int HSPReadConfigFile(HSP *sp);
  int hexToBinary(u_char *hex, u_char *bin, uint32_t binLen);
  int printUUID(const u_char *a, u_char *buf, int bufLen);
  int parseUUID(char *str, char *uuid);
  HSPSFlowSettings *newSFlowSettings(void);
  HSPCollector *newCollector(HSPSFlowSettings *sFlowSettings);
  void freeSFlowSettings(HSPSFlowSettings *sFlowSettings);
  int lookupAddress(char *name, struct sockaddr *sa, SFLAddress *addr, int family);
  
  // using DNS SRV+TXT records
#define SFLOW_DNS_SD "_sflow._udp"
#define HSP_MAX_DNS_LEN 255
  typedef void (*HSPDnsCB)(HSP *sp, uint16_t rtype, uint32_t ttl, u_char *key, int keyLen, u_char *val, int valLen);
  int dnsSD(HSP *sp, HSPDnsCB callback);
  
  // logger
  void myLog(int syslogType, char *fmt, ...);

  // allocation
  void *my_calloc(size_t bytes);
  void *my_realloc(void *ptr, size_t bytes);

  // utils
  char *trimWhitespace(char *str);

  // read functions
  int readInterfaces(HSP *sp);
  int readCpuCounters(SFLHost_cpu_counters *cpu);
  int readMemoryCounters(SFLHost_mem_counters *mem);
  int readDiskCounters(HSP *sp, SFLHost_dsk_counters *dsk);
  int readNioCounters(HSP *sp, SFLHost_nio_counters *dsk, char *devFilter);
  void updateNioCounters(HSP *sp);
  int readHidCounters(HSP *sp, SFLHost_hid_counters *hid, char *hbuf, int hbufLen, char *rbuf, int rbufLen);

  static inline int lockOrDie(pthread_mutex_t *sem) {
    if(sem && pthread_mutex_lock(sem) != 0) {
      myLog(LOG_ERR, "failed to lock semaphore!");
      exit(EXIT_FAILURE);
    }
    return YES;
  }

  static inline int releaseOrDie(pthread_mutex_t *sem) {
    if(sem && pthread_mutex_unlock(sem) != 0) {
      myLog(LOG_ERR, "failed to unlock semaphore!");
      exit(EXIT_FAILURE);
    }
    return YES;
  }

#define STRINGIFY(Y) #Y
#define STRINGIFY_DEF(D) STRINGIFY(D)

#define DYNAMIC_LOCAL(VAR) VAR
#define SEMLOCK_DO(_sem) for(int DYNAMIC_LOCAL(_ctrl)=1; DYNAMIC_LOCAL(_ctrl) && lockOrDie(_sem); DYNAMIC_LOCAL(_ctrl)=0, releaseOrDie(_sem))

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* HSFLOWD_H */

