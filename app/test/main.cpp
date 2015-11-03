#include "base/dcsmq.h"
#include "base/dctcp.h"
#include "base/logger.h"
#include "dcnode/dcnode.h"
#include "dagent/dagent.h"
#include "base/utility.hpp"
#include "base/msg_proto.hpp"

static int max_ping_pong = 1000000;
static int max_ppsz = 0;
static int pingpong = 0;
static const char * msgqpath = nullptr;
static uint64_t start_us = 0;
int mq_cb(dcsmq_t * mq, uint64_t src, const dcsmq_msg_t & msg, void * ud)
{
	//LOGP("mq cb msg size:%d src:%lu", msg.sz, src);
	max_ppsz += msg.sz;
	pingpong++;
	if (pingpong == 1){
		start_us = dcsutil::time_unixtime_us();
	}
	dcsmq_send(mq, src, msg);
	if (pingpong >= max_ping_pong)
	{
		start_us = dcsutil::time_unixtime_us() - start_us;
		LOGP("mq cb msg size:%d src:%lu total:%d MB time:%ldus pingpong:%d",
			msg.sz, src, max_ppsz / 1048576, start_us, max_ping_pong);
		exit(0);
	}
	return 0;
}
#define CHECK(r)	\
if (r) {\
\
	LOGP("check error :%d sys error:%s",r, strerror(errno)); \
	return -1;\
}


int test_mq(const char * ap)
{
	const int test_msg_len = 1024;
	char * test_msg = (char*)malloc(test_msg_len);

	dcsmq_config_t	sc;
	sc.key = msgqpath;
	sc.server_mode = ap ? true : false;
	auto p = dcsmq_create(sc);
	if (!p)
	{
		LOGP("error create smq errno:%s", strerror(errno));
		return -1;
	}
	dcsmq_msg_cb(p, mq_cb, nullptr);
	if (!sc.server_mode)
	{
		start_us = dcsutil::time_unixtime_us();
		pingpong = 1;
		dcsmq_send(p, getpid(), dcsmq_msg_t(test_msg, test_msg_len));
	}
	while (true)
	{
		dcsmq_poll(p, 100);//max proc time is 80-> 12.5 000 * 5 = 1.25W
	}
	return 0;
}
const char * stmsg = "hello,world!";
int _dctcp_cb(dctcp_t* stc, const dctcp_event_t & ev, void * ud)
{
	LOGP("stcp event type:%d fd:%d reason:%d last error msg:%s", ev.type, ev.fd, ev.reason, strerror(ev.error));
	if (ev.type == dctcp_event_type::DCTCP_CONNECTED){
		dctcp_send(stc, ev.fd, dctcp_msg_t(stmsg, strlen(stmsg)+1));
	}
	if (ev.type == dctcp_event_type::DCTCP_READ){
		LOGP("ping pang get msg from fd:%d msg:%s",ev.fd, ev.msg->buff);
		dctcp_send(stc, ev.fd, *ev.msg);
	}
	return -1;
}

int test_tcp(const char * ap)
{
	dctcp_config_t sc;
	sc.is_server = ap ? true : false;
	sc.listen_addr.ip = "127.0.0.1";
	sc.listen_addr.port = 8888;
	auto * p = dctcp_create(sc);
	if (!p)
	{
		LOGP("create stcp error ! syserror:%s", strerror(errno));
		return -1;
	}
	dctcp_event_cb(p, _dctcp_cb, nullptr);
	if (!sc.is_server)
	{
		int ret = dctcp_connect(p, sc.listen_addr, 5);
		CHECK(ret)		
	}
	while (true)
	{
		dctcp_poll(p, 1000);
		usleep(1000);
	}	
	return 0;
}

int dc_cb(void * ud, const char* src, const msg_buffer_t & msg)
{
	LOGP("dc msg recv :%s", msg.buffer);
	return 0;
}

//l3:test callbaker
int test_node(const char * p)
{
	dcnode_config_t dcf;
	dcf.addr.msgq_path = "./gmon.out";
	dcf.addr.msgq_push = true;
	dcf.max_channel_buff_size = 1024 * 1024;
	dcf.name = "leaf";
	dcf.heart_beat_gap = 10;
	dcf.max_live_heart_beat_gap = 20;
	int ltest = 0;
	logger_config_t loger_conf;
	global_logger_init(loger_conf);
	//test auto reconnection 
	//l1->l2
	if (p)
	{
		if (strcmp(p, "l1") == 0){
			dcf.addr.msgq_path = "./gmon.out";
			dcf.addr.msgq_push = false;
			dcf.addr.parent_addr = "127.0.0.1:8880";
			dcf.name = "layer1";
		}
		else
		if (strcmp(p, "l2") == 0){
			dcf.addr.msgq_path = "";
			dcf.name = "layer2";
			dcf.addr.listen_addr = "127.0.0.1:8880";
		}
		else 
		if (strcmp(p, "l3") == 0){
			dcf.addr.msgq_path = "";
			dcf.name = "test";
			dcf.addr.listen_addr = "";
			dcf.heart_beat_gap = 0;
			dcf.max_live_heart_beat_gap = 0;
			ltest = 3;
		}
		else if (strcmp(p, "l4") == 0){
			dcf.name = "leaf2";
			ltest = 4;
		}
	}
	auto dc = dcnode_create(dcf);
	CHECK(!dc)
	dcnode_set_dispatcher(dc, dc_cb, dc);
	int times = 0;
	uint64_t t1, t2;
	if (ltest == 3){
		LOGP("add timer test in node....");
		t1 = dcnode_timer_add(dc, 1000, [&times, &t1, &t2, dc](){
			puts("test timer 1s");
			times++;
			if (times > 5 && t2 > 0){
				puts("cancel t1");
				dcnode_timer_cancel(dc, t2);
				t2 = 0;
			}
			if (times > 10 && t1 > 0){
				puts("cancel t1");
				dcnode_timer_cancel(dc, t1);
				t1 = 0;
			}

		}, true);
		t2 = dcnode_timer_add(dc, 1000 * 3, [](){
			puts("test timer 3s");
		}, true);
	}
	time_t last_time = time(NULL);
	while (true)
	{
		dcnode_update(dc, 10000);
		if (dcnode_ready(dc) == -1){
			LOGP("dcnode stoped ....");
			return -1;
		}
		if (ltest == 4 && last_time + 5 < time(NULL)){
			CHECK(dcnode_send(dc, "leaf", stmsg, strlen(stmsg)))
		}
		usleep(10000);
	}
	return 0;
}
#include "base/script_vm.h"
static int python_test(){
	script_vm_config_t smc;
	smc.type = SCRIPT_VM_PYTHON;
	script_vm_t * vm = script_vm_create(smc);
	if (!vm){
		LOGP("error create vm !");
		return -1;
	}
	int r = script_vm_run_string(vm, "a=20");
	r |= script_vm_run_string(vm, "print('hello,world!')");
	r |= script_vm_run_string(vm, "print('hello,world!'+str(a))");
	LOGP("vm run ret:%d",r);
	script_vm_destroy(vm);

	LOGP("destroyed vm");
	smc.path = "./plugins";
	vm = script_vm_create(smc);
	if (!vm){
		LOGP("error create vm agin !");
		return -1;
	}
	r = script_vm_run_string(vm, "a=20");
	r |= script_vm_run_string(vm, "print('hello,world! %s' % a)");
	LOGP("vm run ret:%d", r);
	r = script_vm_run_file(vm, "hello.py");
	LOGP("vm run ret:%d", r);

	return 0;
}
int log_test(){
	logger_config_t lc;
	lc.max_file_size = 1024;
	lc.max_roll = 3;
	lc.path = "./";
	lc.pattern = "test.log";
	int ret = global_logger_init(lc);
	if (ret){
		return ret;
	}
	int n = 3 * 1000;
	while (n--){
		GLOG(LOG_LVL_ERROR, ret, "test logger msg just for size , this is dummy");
	}
	return 0;
}
#include "dcnode/proto/dcnode.pb.h"
int perf_test(const char * arg){
	
	typedef msgproto_t<dcnode::MsgDCNode>	msg_t;
	start_us = dcsutil::time_unixtime_us();
	int pack_unpack_times = 1000000;
	msg_buffer_t msgbuf;
	const char * s_test_msg = "hello,worlddfffxxxxxfggggggggggggggdfxsfsddddddddddddddddddfxxxxxxxxxxxxxxxffffffffffffffffffffffffff";
	msgbuf.create(10240);
	uint64_t packsize = 0;
	for (int i = 0; i < pack_unpack_times; ++i){
		msg_t	msg;
		msg.set_dst("hello");
		msg.set_src("heffff");
		msg.set_type(dcnode::MSG_DATA);
		msg.set_msg_data(s_test_msg, strlen(s_test_msg)+1);
		msg.mutable_ext()->set_unixtime(dcsutil::time_unixtime_ms() / 1000);
		if (!msg.Pack(msgbuf))
		{
			printf("error pack!\n");
		}
		if (!msg.Unpack(msgbuf)){
			printf("error unpack!\n");
		}
		//
		packsize += msg.PackSize();
	}
	int64_t cost_time = dcsutil::time_unixtime_us() - start_us;
	double speed = pack_unpack_times*1000000.0 / cost_time ;
	LOGP("print pack size:%lu msg size:%zd cost time:%ld total times:%d speed:%lf /s", 
		packsize, strlen(s_test_msg)+1,
		cost_time,
		pack_unpack_times, speed);
	return 0;
}


int main(int argc, char* argv[])
{
	global_logger_init(logger_config_t());
	int agent_mode = 0;
	msgqpath = argv[0];
	if (argc >= 2)
	{
		switch (argv[1][0])
		{
		case 'm':
			return test_mq(argv[2]);
		case 't':
			return test_tcp(argv[2]);
		case 'n':
			return test_node(argv[2]);
		case 'a':
			agent_mode = 1;
			break;
		case 'p':
			return python_test();
		case 'l':
			return log_test();
		case 'f':
			return perf_test(argv[1]);
		default:
			break;
		}
	}

	dagent_config_t	conf;
	conf.name = "libagent";
	conf.localkey = "./dcagent";
	conf.heartbeat = 10;
	//an agent
	if (agent_mode){
		conf.name = "binagent";
		conf.routermode = true;
	}
	int ret = dagent_init(conf);
	if (ret)
	{
		return -1;
	}
	while (true)
	{
		dagent_update();
		usleep(1000);//1ms
	}
	dagent_destroy();
	return 0;
}
