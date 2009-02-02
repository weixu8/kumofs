#include "logic/base.h"
#include "logic/srv.h"
#include <fstream>

using namespace kumo;

struct arg_t : rpc_cluster_args {

	std::string dbpath;

	sockaddr_in manager1_in;
	sockaddr_in manager2_in;
	bool manager2_set;
	rpc::address manager1;  // convert
	rpc::address manager2;  // convert

	uint16_t stream_port;
	rpc::address stream_addr;  // convert
	int stream_lsock;

	std::string offer_tmpdir;

	Storage* db;
	std::string db_backup_basename;  // convert?

	unsigned short replicate_set_retry_num;
	unsigned short replicate_delete_retry_num;

	virtual void convert()
	{
		cluster_addr = rpc::address(cluster_addr_in);
		cluster_lsock = scoped_listen_tcp::listen(cluster_addr);
		stream_addr = cluster_addr;
		stream_addr.set_port(stream_port);
		stream_lsock = scoped_listen_tcp::listen(stream_addr);
		manager1 = rpc::address(manager1_in);
		if(manager2_set) {
			manager2 = rpc::address(manager2_in);
			if(manager2 == manager1) {
				throw std::runtime_error("-m and -p must be different");
			}
		}
		db_backup_basename = dbpath + "-";
		rpc_cluster_args::convert();
	}

	arg_t(int argc, char** argv) :
		stream_port(SERVER_STREAM_DEFAULT_PORT),
		replicate_set_retry_num(20),
		replicate_delete_retry_num(20)
	{
		clock_interval = 8.0;

		using namespace kazuhiki;
		set_basic_args();
		on("-l",  "--listen",
				type::connectable(&cluster_addr_in, SERVER_DEFAULT_PORT));
		on("-L", "--stream-listen",
				type::numeric(&stream_port, stream_port));
		on("-f", "--offer-tmp",
				type::string(&offer_tmpdir, "/tmp"));
		on("-s", "--store",
				type::string(&dbpath));
		on("-m", "--manager1",
				type::connectable(&manager1_in, MANAGER_DEFAULT_PORT));
		on("-p", "--manager2", &manager2_set,
				type::connectable(&manager2_in, MANAGER_DEFAULT_PORT));
		on("-S", "--replicate-set-retry",
				type::numeric(&replicate_set_retry_num, replicate_set_retry_num));
		on("-G", "--replicate-delete-retry",
				type::numeric(&replicate_delete_retry_num, replicate_delete_retry_num));
		parse(argc, argv);
	}

	void show_usage()
	{
std::cout <<
"usage: "<<prog<<" -m <addr[:port="<<MANAGER_DEFAULT_PORT<<"]> -p <addr[:port"<<MANAGER_DEFAULT_PORT<<"]> -l <addr[:port="<<SERVER_DEFAULT_PORT<<"]> -s <path.tch>\n"
"\n"
"  -l  <addr[:port="<<SERVER_DEFAULT_PORT<<"]>   "        "--listen         listen address\n"
"  -L  <port="<<SERVER_STREAM_DEFAULT_PORT<<">          " "--stream-listen  listen port for replacing stream\n"
"  -f  <dir="<<"/tmp"<<">            "                    "--offer-tmp      path to temporary directory for replacing\n"
"  -s  <path.tch>            "                            "--store          path to database\n"
"  -m  <addr[:port="<<MANAGER_DEFAULT_PORT<<"]>   "       "--manager1       address of manager 1\n"
"  -p  <addr[:port="<<MANAGER_DEFAULT_PORT<<"]>   "       "--manager2       address of manager 2\n"
"  -S  <number="<<replicate_set_retry_num<<">   "         "--replicate-set-retry    replicate set retry limit\n"
"  -D  <number="<<replicate_delete_retry_num<<">   "      "--replicate-delete-retry replicate delete retry limit\n"
;
rpc_cluster_args::show_usage();
	}
};

int main(int argc, char* argv[])
{
	arg_t arg(argc, argv);

	// initialize logger first
	mlogger::level loglevel = (arg.verbose ? mlogger::TRACE : mlogger::WARN);
	if(arg.logfile_set) {
		if(arg.logfile == "-") {
			mlogger::reset(new mlogger_ostream(loglevel, std::cout));
		} else {
			std::ostream* logstream = new std::ofstream(arg.logfile.c_str(), std::ios::app);
			mlogger::reset(new mlogger_ostream(loglevel, *logstream));
		}
	} else if(arg.pidfile_set) {
		// log to stdout
		mlogger::reset(new mlogger_ostream(loglevel, std::cout));
	} else {
		// log to tty
		mlogger::reset(new mlogger_tty(loglevel, std::cout));
	}

	// daemonize
	if(!arg.pidfile.empty()) {
		do_daemonize(!arg.logfile.empty(), arg.pidfile.c_str());
	}

	// initialize binary logger
	if(arg.logpack_path_set) {
		logpacker::initialize(arg.logpack_path.c_str());
	}

	// open database
	std::auto_ptr<Storage> db(new Storage(arg.dbpath.c_str()));
	arg.db = db.get();

	// run server
	Server::initialize(arg);
	Server::instance().run();
	Server::instance().join();

	return 0;
}

