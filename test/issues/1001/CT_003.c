#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#include "ihklib.h"
#include "./test_chk.h"

#define TEST_NAME "CT_003"

#define NUM_CNTR 1

int main(int argc, char **argv)
{
	struct ihk_perf_event_attr attr[NUM_CNTR];
	// for x86_64 arch
	unsigned long configs[NUM_CNTR] = {
		0x00c0};
	char *config_names[NUM_CNTR] = {
		"INSTRUCTIONS"};
	unsigned long counts[NUM_CNTR];
	unsigned long tmp_counts[NUM_CNTR];

	int rc = 0;
	int i = 0;
	int chk_fail = 0;
	int event_num;

	printf("*** %s start *******************************\n", TEST_NAME);

	// setup attrs
	for (i = 0; i < NUM_CNTR; i++) {
		attr[i].config = configs[i];
		attr[i].exclude_kernel = 1;
		attr[i].exclude_user = 0;
		attr[i].disabled = 1;
	}

	// set perf_event
	rc = ihk_os_setperfevent(0, attr, NUM_CNTR);
	OKNG(rc < 0, "setperfevent for %d counter", NUM_CNTR);
	event_num = rc;

	// start counters at once
	rc = ihk_os_perfctl(0, PERF_EVENT_ENABLE);
	OKNG(rc != 0, "Start counter");

	// monitoring target
	printf("  do some processing...\n");
	system("bash ./processing.sh > /dev/null");

	// stop counters at once
	rc = ihk_os_perfctl(0, PERF_EVENT_DISABLE);
	OKNG(rc != 0, "Stop counter");

	rc = ihk_os_getperfevent(0, tmp_counts, event_num);
	OKNG(rc != 0, "getperfevent %d counter", event_num);
	printf("  counted value is as belows...\n");
	// read counters after processing
	for (i = 0; i < NUM_CNTR; i++) {
		printf("%-16s: %ld\n", config_names[i], tmp_counts[i]);
		counts[i] = tmp_counts[i];
	}

	printf("  processing again... (to check if counter is stopped)\n");
	// processing again (counters are stopped)
	system("bash ./processing.sh > /dev/null");

	rc = ihk_os_getperfevent(0, tmp_counts, event_num);
	OKNG(rc != 0, "getperfevent %d counter", event_num);
	printf("  current value is as bellow\n"
		"  (expected to be same value as last time)\n");

	// read counters again to check if counters were stopped
	chk_fail = 0;
	for (i = 0; i < NUM_CNTR; i++) {
		printf("%-16s: %ld\n", config_names[i], tmp_counts[i]);
		if (counts[i] != tmp_counts[i]) {
			chk_fail = 1;
			break;
		}
	}
	OKNG(chk_fail != 0, "Counter is stopped");

	printf("*** %s PASSED\n\n", TEST_NAME);

	return 0;

fn_fail:
	printf("*** %s FAILED\n\n", TEST_NAME);

	return -1;
}
