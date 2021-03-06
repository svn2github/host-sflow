/* This software is distributed under the following license:
 * http://host-sflow.sourceforge.net/license.html
 */


#if defined(__cplusplus)
extern "C" {
#endif

#include "hsflowd.h"
#include "readWindowsEnglishCounters.h"

extern int debug;

/**
 * Populates the host_memory structure using data retrieved using
 * GlobalMemoryStatusEx function and the Memory performance counter
 * object.
 * Returns FALSE if call to GlobalMemoryStatusEx produces an error, TRUE otherwise.
 * Note that the Windows use of memory and classification of use does
 * not map cleanly to Linux terms.
 * We are using Windows available memory for mem_free. Available memory
 * includes standby memory (memory removed from a process's working set 
 * - its physical memory - on route to disk, but is still available to be recalled)
 * and free and zero page list bytes. Windows Resource Monitor reports free memory 
 * as free page list bytes.
 * Memory\\Cache Bytes is used for mem_cached since this really does appear to be
 * the Linux equivalent of cache (file system cache).
 * Windows Resource Monitor reports cached as Standby+Modified this is not the
 * equivalent of Linux file system cache.
 * Windows also does not seem to report swapping (all memory associated with a process
 * swapped in/out of memory). Memory\\Pages Input/sec and Memory\\Pages Output/sec
 * are used for page_in and page_out.
 */
BOOL readMemoryCounters(SFLHost_mem_counters *mem) 
{
	MEMORYSTATUSEX memStat;
	memStat.dwLength = sizeof(memStat);
	if (GlobalMemoryStatusEx(&memStat) == 0){
		myLog(LOG_ERR,"GlobalMemoryStatusEx failed: %d\n",GetLastError());
		return FALSE;
	}
	mem->mem_total = memStat.ullTotalPhys;
	mem->mem_free = memStat.ullAvailPhys;
	mem->swap_total = memStat.ullTotalPageFile;
	mem->swap_free = memStat.ullAvailPageFile;
	PDH_HQUERY query;
	if (PdhOpenQuery(NULL, 0, &query) == ERROR_SUCCESS) {
		PDH_HCOUNTER cache, pageIn, pageOut;
		if (addCounterToQuery(MEM_COUNTER_OBJECT, NULL, MEM_COUNTER_CACHE, &query, &cache) == ERROR_SUCCESS &&
			addCounterToQuery(MEM_COUNTER_OBJECT, NULL, MEM_COUNTER_PAGE_IN, &query, &pageIn) == ERROR_SUCCESS &&
			addCounterToQuery(MEM_COUNTER_OBJECT, NULL, MEM_COUNTER_PAGE_OUT, &query, &pageOut) == ERROR_SUCCESS &&
			PdhCollectQueryData(query) == ERROR_SUCCESS) {
			mem->mem_cached= getRawCounterValue(&cache);
			mem->page_in = (uint32_t)getRawCounterValue(&pageIn);
			mem->page_out = (uint32_t)getRawCounterValue(&pageOut);
		}
		PdhCloseQuery(query);
	}

	//There are no obvious Windows equivalents
    mem->mem_shared = UNKNOWN_COUNTER_64;
	mem->mem_buffers = UNKNOWN_COUNTER_64;
	//using the definition that swapping is when all the memory associated with a
	//process is moved in/out of RAM
	mem->swap_in = UNKNOWN_COUNTER;
	mem->swap_out = UNKNOWN_COUNTER;
	myLog(LOG_INFO,
		"readMemoryCounters:\n\ttotal: \t\t%I64d\n\tfree: \t\t%I64d\n"
		"\tcached: \t%I64d\n\tpage_in: \t%d\n\tpage_out: \t%d\n"
		"\tswap_total: \t%I64d\n\tswap_free: \t%I64d\n",
		mem->mem_total,mem->mem_free,
		mem->mem_cached,mem->page_in,mem->page_out,
		mem->swap_total, mem->swap_free);
	return TRUE;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif