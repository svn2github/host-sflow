/* This software is distributed under the following license:
 * http://host-sflow.sourceforge.net/license.html
 */

#ifndef SFLOWOVSD_H
#define SFLOWOVSD_H 1

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
#include <sys/stat.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>

#include <sys/wait.h>

#include <sys/types.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h> // for PRIu64 etc.
#include "malloc.h" // for malloc_stats()
#include "sflow_api.h"

#define YES 1
#define NO 0

#define SFVS_VERSION "0.9"
#define SFVS_DAEMON_NAME "sflowovsd"
#define SFVS_DEFAULT_PIDFILE "/var/run/sflowovsd.pid"
#define SFVS_DEFAULT_CONFIGFILE "/etc/hsflowd.auto"
#define SFVS_MAX_TICKS 60

  typedef struct _SFVSStringArray {
    char **strs;
    uint32_t n;
    uint32_t capacity;
  } SFVSStringArray;

  typedef enum { SFVSSTATE_INIT=0,
		 SFVSSTATE_READCONFIG,
		 SFVSSTATE_READCONFIG_FAILED,
		 SFVSSTATE_SYNC,
		 SFVSSTATE_SYNC_SEARCH,
		 SFVSSTATE_SYNC_FOUND,
		 SFVSSTATE_SYNC_DESTROY,
		 SFVSSTATE_SYNC_FAILED,
		 SFVSSTATE_SYNC_OK,
		 SFVSSTATE_END,
  } EnumSFVSState;

#ifdef SFLOWOVSD_MAIN
  static const char *SFVSStateNames[] = {
    "INIT",
    "READCONFIG",
    "READCONFIG_FAILED",
    "SYNC",
    "SYNC_SEARCH",
    "SYNC_FOUND",
    "SYNC_DESTROY",
    "SYNC_FAILED",
    "SYNC_OK",
    "END"
  };
#endif

#define SFVS_SEPARATORS " \t\r\n="
#define SFVS_QUOTES "'\" \t\r\n"
// SFVS_MAX LINE LEN must be enough to hold the whole list of targets
#define SFVS_MAX_LINELEN 1024
#define SFVS_MAX_COLLECTORS 10

  typedef struct _SFVSCollector {
    char *ip;
    uint16_t port;
    uint16_t priority;
  } SFVSCollector;

  typedef struct _SFVSConfig {
    int error;
    uint32_t sampling_n;
    uint32_t polling_secs;
    uint32_t header_bytes;
    char *agentIP;
    uint32_t num_collectors;
    SFVSCollector collectors[SFVS_MAX_COLLECTORS];
    SFVSStringArray *targets;
    char *targetStr;
  } SFVSConfig;

#define SFVS_OVS_CMD "/usr/bin/ovs-vsctl"
// new sflow id must start with '@'
#define SFVS_NEW_SFLOW_ID "@newsflow"

  typedef struct _SFVS {
    EnumSFVSState state;
    time_t tick;
    char *configFile;
    time_t configFile_modTime;
    char *pidFile;
    SFVSConfig config;
    SFVSStringArray *cmd;
    SFVSStringArray *extras;
    char *bridge;
    char *sflowUUID;
    int cmdFailed;
  } SFVS;
  
  // logger
  void myLog(int syslogType, char *fmt, ...);

  // allocation
  void *my_calloc(size_t bytes);

  // myExec
  typedef int (*SFVSExecCB)(SFVS *sv, char *line);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* SFLOWOVSD_H */

