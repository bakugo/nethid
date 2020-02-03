#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "switch.h"

extern "C" {
	extern u32 __start__;
	
	u32 __nx_applet_type = AppletType_None;
	u32 __nx_fs_num_sessions = 1;
	u32 __nx_fsdev_direntry_cache_size = 1;
	
	#define INNER_HEAP_SIZE 0x40000
	size_t nx_inner_heap_size = INNER_HEAP_SIZE;
	char nx_inner_heap[INNER_HEAP_SIZE];
	
	void __libnx_initheap(void);
	
	void userAppInit(void);
	void userAppExit(void);
}

int main(int argc, char **argv);
void main_control(void* _);
void main_network(void* _);
void receive();
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void __libnx_initheap(void) {
	void* addr = nx_inner_heap;
	size_t size = nx_inner_heap_size;
	
	extern char* fake_heap_start;
	extern char* fake_heap_end;
	
	fake_heap_start = (char*)addr;
	fake_heap_end = (char*)addr + size;
}

static const SocketInitConfig socket_conf = {
	.bsdsockets_version = 1,
	
	.tcp_tx_buf_size = 0x200,
	.tcp_rx_buf_size = 0x400,
	.tcp_tx_buf_max_size = 0x400,
	.tcp_rx_buf_max_size = 0x800,
	
	.udp_tx_buf_size = 0x2400,
	.udp_rx_buf_size = 0xA500,
	
	.sb_efficiency = 2,
};

void userAppInit(void) {
	Result res;
	
	svcSleepThread(2e+8L);
	
	if (hosversionBefore(9, 0, 0)) {
		fatalThrow(0);
	}
	
	res = hiddbgInitialize();
	if (R_FAILED(res)) fatalThrow(res);
	
	res = hiddbgAttachHdlsWorkBuffer();
	if (R_FAILED(res)) fatalThrow(res);
	
	res = socketInitialize(&socket_conf);
	if (R_FAILED(res)) fatalThrow(res);
}

void userAppExit(void) {
	hiddbgReleaseHdlsWorkBuffer();
	hiddbgExit();
	socketExit();
}

#define COLOR(a) (((a&0xFF0000)>>16)|(a&0x00FF00)|((a&0x0000FF)<<16)|(0xFF<<24))

struct __attribute__((__packed__)) packet_t {
	u16 magic;
	u64 keys;
	s32 joy_l_x;
	s32 joy_l_y;
	s32 joy_r_x;
	s32 joy_r_y;
};

static Mutex lock_log = 0;
static Mutex lock_net = 0;

static Thread thr_control;
static Thread thr_network;

static u64 hdls_handle;
static HiddbgHdlsDeviceInfo hdls_devinfo;
static HiddbgHdlsState hdls_state;

static struct packet_t packet;

static int socket_fd = -1;
static struct sockaddr_in socket_addr_sv;
static struct sockaddr_in socket_addr_cl;
static u32 socket_last_host = 0;
static u64 socket_last_time = 0;
static u64 socket_failures = 0;

int main(int argc, char** argv) {
	Result res;
	
	svcSleepThread(2*1000000000L);
	
	log("init");
	
	svcSleepThread(1*1000000000L);
	
	log("attaching controller");
	
	hdls_handle = 0;
	hdls_devinfo = {0};
	hdls_state = {0};
	
	// make a wired pro controller
	hdls_devinfo.deviceType = HidDeviceType_FullKey15;
	hdls_devinfo.npadInterfaceType = NpadInterfaceType_USB;
	
	// set controller colors
	hdls_devinfo.singleColorBody = COLOR(0x3995C6);
	hdls_devinfo.singleColorButtons = COLOR(0x202020);
	hdls_devinfo.colorLeftGrip = COLOR(0x3995C6);
	hdls_devinfo.colorRightGrip = COLOR(0x3995C6);
	
	// init input state
	hdls_state.batteryCharge = 4;
	hdls_state.flags = 0;
	hdls_state.buttons = 0;
	hdls_state.joysticks[JOYSTICK_LEFT].dx = 0;
	hdls_state.joysticks[JOYSTICK_LEFT].dy = 0;
	hdls_state.joysticks[JOYSTICK_RIGHT].dx = 0;
	hdls_state.joysticks[JOYSTICK_RIGHT].dy = 0;
	
	res = hiddbgAttachHdlsVirtualDevice(&hdls_handle, &hdls_devinfo);
	
	if (R_FAILED(res)) {
		log("failed to attach controller");
		fatalThrow(res);
	}
	
	threadCreate(&thr_control, main_control, NULL, NULL, 0x1000, 0x21, 3);
	threadCreate(&thr_network, main_network, NULL, NULL, 0x1000, 0x30, 3);
	
	threadStart(&thr_control);
	threadStart(&thr_network);
	
	while (appletMainLoop()) {
		svcSleepThread(1e+9L / 10);
	}
	
	return 0;
}

void main_control(void* _) {
	Result res;
	
	while (true) {
		hdls_state.buttons = 0;
		hdls_state.joysticks[JOYSTICK_LEFT].dx = 0;
		hdls_state.joysticks[JOYSTICK_LEFT].dy = 0;
		hdls_state.joysticks[JOYSTICK_RIGHT].dx = 0;
		hdls_state.joysticks[JOYSTICK_RIGHT].dy = 0;
		
		mutexLock(&lock_net);
		
		if (packet.magic != 0) {
			hdls_state.buttons = packet.keys;
			hdls_state.joysticks[JOYSTICK_LEFT].dx = packet.joy_l_x;
			hdls_state.joysticks[JOYSTICK_LEFT].dy = packet.joy_l_y;
			hdls_state.joysticks[JOYSTICK_RIGHT].dx = packet.joy_r_x;
			hdls_state.joysticks[JOYSTICK_RIGHT].dy = packet.joy_r_y;
		}
		
		mutexUnlock(&lock_net);
		
		res = hiddbgSetHdlsState(hdls_handle, &hdls_state);
		
		if (R_FAILED(res)) {
			log("failed to set input state");
			fatalThrow(res);
		}
		
		svcSleepThread(1e+9L / ((60*3)+1));
	}
}

void main_network(void* _) {
	while (true) {
		receive();
		
		if (packet.magic == 0) {
			svcSleepThread(1e+7L);
		}
		
		svcSleepThread(-1);
	}
}

void receive() {
	bool init;
	struct timeval timeout;
	struct packet_t packet_tmp;
	int bytes;
	socklen_t addrlen;
	
	init = false;
	
	if (socket_fd == -1) {
		log("creating socket");
		
		init = true;
	}
	
	if (
		init == false &&
		(svcGetSystemTick() - socket_last_time) > (19200000 / 3)
	) {
		log("resetting socket due to sleep");
		
		svcSleepThread(5e+8L);
		
		init = true;
	}
	
	if (
		init == false &&
		socket_last_host != gethostid()
	) {
		log("resetting socket due to new host");
		
		init = true;
	}
	
	if (init) {
		if (socket_fd != -1) {
			close(socket_fd);
		}
		
		if ((socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			log("failed to create socket");
			fatalThrow(0);
		}
		
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;
		
		setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		
		memset(&socket_addr_sv, 0, sizeof(socket_addr_sv));
		memset(&socket_addr_cl, 0, sizeof(socket_addr_cl));
		
		socket_addr_sv.sin_family = AF_INET;
		socket_addr_sv.sin_addr.s_addr = INADDR_ANY;
		socket_addr_sv.sin_port = htons(8080);
		
		bind(socket_fd, (const struct sockaddr*)&socket_addr_sv, sizeof(socket_addr_sv));
	}
	
	socket_last_host = gethostid();
	socket_last_time = svcGetSystemTick();
	
	if (
		socket_failures > 30 &&
		socket_failures % 5 != 0
	) {
		socket_failures++;
		return;
	}
	
	packet_tmp.magic = 0;
	
	bytes =
		recvfrom(
			socket_fd,
			&packet_tmp,
			sizeof(struct packet_t),
			MSG_WAITALL,
			(struct sockaddr*)&socket_addr_cl,
			&addrlen
		);
	
	mutexLock(&lock_net);
	
	if (
		bytes > 0 &&
		packet_tmp.magic == 0x3275
	) {
		packet = packet_tmp;
		socket_failures = 0;
	} else {
		socket_failures++;
		
		if (socket_failures > 5) {
			packet.magic = 0;
		}
	}
	
	mutexUnlock(&lock_net);
}

void log(const char *fmt, ...) {
	return;
	
	mutexLock(&lock_log);
	
	time_t time_unix = time(NULL);
	struct tm time;
	localtime_r(&time_unix, &time);
	
	FILE *fp = fopen("/_nethid.txt", "a");
	
	fprintf(fp, "[%04i-%02i-%02i %02i:%02i:%02i] ", (time.tm_year + 1900), (time.tm_mon + 1), time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
	
	va_list va;
	va_start(va, fmt);
	vfprintf(fp, fmt, va);
	va_end(va);
	
	fprintf(fp, "\n");
	
	fclose(fp);
	
	mutexUnlock(&lock_log);
}
