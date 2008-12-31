#include "gateway/memtext.h"
#include "memproto/memtext.h"
#include <mp/pthread.h>
#include <mp/stream_buffer.h>
#include <mp/object_callback.h>
#include <stdexcept>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <memory>

namespace kumo {


static const size_t MEMTEXT_INITIAL_ALLOCATION_SIZE = 16*1024;
static const size_t MEMTEXT_RESERVE_SIZE = 1024;


MemprotoText::MemprotoText(int lsock) :
	m_lsock(lsock) { }

MemprotoText::~MemprotoText() {}


void MemprotoText::accepted(Gateway* gw, int fd, int err)
{
	if(fd < 0) {
		LOG_FATAL("accept failed: ",strerror(err));
		gw->signal_end(SIGTERM);
		return;
	}
	LOG_DEBUG("accept memproto text user fd=",fd);
	wavy::add<Connection>(fd, gw);
}

void MemprotoText::listen(Gateway* gw)
{
	using namespace mp::placeholders;
	wavy::listen(m_lsock,
			mp::bind(&MemprotoText::accepted, gw, _1, _2));
}


class MemprotoText::Connection : public wavy::handler {
public:
	Connection(int fd, Gateway* gw);
	~Connection();

public:
	void read_event();

private:
	inline int memproto_get(
			const char** keys, unsigned* key_lens, unsigned key_num);

	inline int memproto_set(
			const char* key, unsigned key_len,
			unsigned short flags, uint32_t exptime,
			const char* data, unsigned data_len,
			bool noreply);

	inline int memproto_delete(
			const char* key, unsigned key_len,
			uint32_t exptime, bool noreply);

private:
	memtext_parser m_memproto;
	mp::stream_buffer m_buffer;
	size_t m_off;
	Gateway* m_gw;

	typedef mp::shared_ptr<bool> SharedValid;
	SharedValid m_valid;

	typedef Gateway::get_request get_request;
	typedef Gateway::set_request set_request;
	typedef Gateway::delete_request delete_request;

	typedef Gateway::get_response get_response;
	typedef Gateway::set_response set_response;
	typedef Gateway::delete_response delete_response;

	typedef rpc::shared_zone shared_zone;


	struct Responder {
		Responder(int fd, SharedValid& valid) :
			m_fd(fd), m_valid(valid) { }
		~Responder() { }

		bool is_valid() const { return *m_valid; }

		int fd() const { return m_fd; }

	protected:
		inline void send_data(const char* buf, size_t buflen);
		inline void send_datav(struct iovec* vb, size_t count, shared_zone& life);

	private:
		int m_fd;
		SharedValid m_valid;
	};

	struct ResGet : Responder {
		ResGet(int fd, SharedValid& valid) :
			Responder(fd, valid) { }
		~ResGet() { }
		void response(get_response& res);
	};

	struct ResMultiGet : Responder {
		ResMultiGet(int fd, SharedValid& valid, size_t count) :
			Responder(fd, valid), m_count(count) { }
		~ResMultiGet() { }
		void response(get_response& res);
	private:
		mp::pthread_mutex m_mutex;
		size_t m_count;
	};

	struct ResSet : Responder {
		ResSet(int fd, SharedValid& valid) :
			Responder(fd, valid) { }
		~ResSet() { }
		void response(set_response& res);
		void no_response(set_response& res);
	};

	struct ResDelete : Responder {
		ResDelete(int fd, SharedValid& valid) :
			Responder(fd, valid) { }
		~ResDelete() { }
		void response(delete_response& res);
		void no_response(delete_response& res);
	};

private:
	Connection();
	Connection(const Connection&);
};


MemprotoText::Connection::Connection(int fd, Gateway* gw) :
	mp::wavy::handler(fd),
	m_buffer(MEMTEXT_INITIAL_ALLOCATION_SIZE),
	m_off(0),
	m_gw(gw),
	m_valid(new bool(true))
{
	int (*cmd_get)(void*, const char**, unsigned*, unsigned) =
		&mp::object_callback<int (const char**, unsigned*, unsigned)>::
				mem_fun<Connection, &Connection::memproto_get>;

	int (*cmd_set)(void*, const char*, unsigned,
			unsigned short, uint32_t,
			const char*, unsigned, bool) =
		&mp::object_callback<int (const char*, unsigned,
				unsigned short, uint32_t,
				const char*, unsigned,
				bool)>::
				mem_fun<Connection, &Connection::memproto_set>;

	int (*cmd_delete)(void*, const char*, unsigned,
			uint32_t, bool) =
		&mp::object_callback<int (const char*, unsigned,
				uint32_t, bool)>::
				mem_fun<Connection, &Connection::memproto_delete>;

	memtext_callback cb = {
		cmd_get,     // get
		cmd_set,     // set
		NULL,        // replace
		NULL,        // append
		NULL,        // prepend
		NULL,        // cas
		cmd_delete,  // delete
	};

	memtext_init(&m_memproto, &cb, this);
}

MemprotoText::Connection::~Connection()
{
	*m_valid = false;
}


void MemprotoText::Connection::read_event()
try {
	m_buffer.reserve_buffer(MEMTEXT_RESERVE_SIZE);

	ssize_t rl = ::read(fd(), m_buffer.buffer(), m_buffer.buffer_capacity());
	if(rl < 0) {
		if(errno == EAGAIN || errno == EINTR) {
			return;
		} else {
			throw std::runtime_error("read error");
		}
	} else if(rl == 0) {
		throw std::runtime_error("connection closed");
	}

	m_buffer.buffer_consumed(rl);

	do {
		int ret = memtext_execute(&m_memproto,
				(char*)m_buffer.data(), m_buffer.data_size(), &m_off);
		if(ret < 0) {
			throw std::runtime_error("parse error");
		}
		if(ret == 0) { return; }
		m_buffer.data_used(m_off);
		m_off = 0;
	} while(m_buffer.data_size() > 0);

} catch (std::exception& e) {
	LOG_DEBUG("memcached text protocol error: ",e.what());
	throw;
} catch (...) {
	LOG_DEBUG("memcached text protocol error: unknown error");
	throw;
}


void MemprotoText::Connection::Responder::send_data(
		const char* buf, size_t buflen)
{
	wavy::write(m_fd, buf, buflen);
}

void MemprotoText::Connection::Responder::send_datav(
		struct iovec* vb, size_t count, shared_zone& life)
{
	wavy::request req(&mp::object_delete<shared_zone>, new shared_zone(life));
	wavy::writev(m_fd, vb, count, req);
}


namespace {
static const char* const NOT_SUPPORTED_REPLY = "CLIENT_ERROR supported\r\n";
static const char* const GET_FAILED_REPLY    = "SERVER_ERROR get failed\r\n";
static const char* const STORE_FAILED_REPLY  = "SERVER_ERROR store failed\r\n";
static const char* const DELETE_FAILED_REPLY = "SERVER_ERROR delete failed\r\n";
}  // noname namespace

#define RELEASE_REFERENCE(life) \
	shared_zone life(new msgpack::zone()); \
	life->push_finalizer(&mp::object_delete<mp::stream_buffer::reference>, \
				m_buffer.release());

int MemprotoText::Connection::memproto_get(
		const char** keys, unsigned* key_lens, unsigned key_num)
{
	LOG_TRACE("get");
	RELEASE_REFERENCE(life);

	if(key_num == 1) {
		const char* key = keys[0];
		unsigned keylen = key_lens[0];

		ResGet* ctx = life->allocate<ResGet>(fd(), m_valid);
		get_request req;
		req.key = key;
		req.keylen = keylen;
		req.hash = Gateway::stdhash(req.key, req.keylen);
		req.callback = &mp::object_callback<void (get_response&)>
			::mem_fun<ResGet, &ResGet::response>;
		req.user = (void*)ctx;
		req.life = life;

		m_gw->submit(req);

	} else {
		size_t keylen_sum = 0;
		for(unsigned i=0; i < key_num; ++i) {
			keylen_sum += key_lens[i];
		}

		ResMultiGet* ctx = life->allocate<ResMultiGet>(fd(), m_valid, key_num);
		get_request req;
		req.callback = &mp::object_callback<void (get_response&)>
			::mem_fun<ResMultiGet, &ResMultiGet::response>;
		req.user = (void*)ctx;
		req.life = life;

		for(unsigned i=0; i < key_num; ++i) {
			req.key = keys[i];
			req.keylen = key_lens[i];
			req.hash = Gateway::stdhash(req.key, req.keylen);
			// FIXME shared zone. msgpack::allocate is not thread-safe.
			m_gw->submit(req);
		}
	}

	return 0;
}


int MemprotoText::Connection::memproto_set(
		const char* key, unsigned key_len,
		unsigned short flags, uint32_t exptime,
		const char* data, unsigned data_len,
		bool noreply)
{
	LOG_TRACE("set");
	RELEASE_REFERENCE(life);

	if(flags || exptime) {
		wavy::write(fd(), NOT_SUPPORTED_REPLY, strlen(NOT_SUPPORTED_REPLY));
		return 0;
	}

	ResSet* ctx = life->allocate<ResSet>(fd(), m_valid);
	set_request req;
	req.key = key;
	req.keylen = key_len;
	req.hash = Gateway::stdhash(req.key, req.keylen);
	req.val = data;
	req.vallen = data_len;
	req.life = life;

	if(noreply) {
		req.callback = &mp::object_callback<void (set_response&)>
			::mem_fun<ResSet, &ResSet::no_response>;
	} else {
		req.callback = &mp::object_callback<void (set_response&)>
			::mem_fun<ResSet, &ResSet::response>;
	}
	req.user = ctx;

	m_gw->submit(req);

	return 0;
}


int MemprotoText::Connection::memproto_delete(
		const char* key, unsigned key_len,
		uint32_t exptime, bool noreply)
{
	LOG_TRACE("delete");
	RELEASE_REFERENCE(life);

	if(exptime) {
		wavy::write(fd(), NOT_SUPPORTED_REPLY, strlen(NOT_SUPPORTED_REPLY));
		return 0;
	}

	ResDelete* ctx = life->allocate<ResDelete>(fd(), m_valid);
	delete_request req;
	req.key = key;
	req.keylen = key_len;
	req.hash = Gateway::stdhash(req.key, req.keylen);
	req.life = life;

	if(noreply) {
		req.callback = &mp::object_callback<void (delete_response&)>
			::mem_fun<ResDelete, &ResDelete::no_response>;
	} else {
		req.callback = &mp::object_callback<void (delete_response&)>
			::mem_fun<ResDelete, &ResDelete::response>;
	}
	req.user = ctx;

	m_gw->submit(req);

	return 0;
}



void MemprotoText::Connection::ResGet::response(get_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("get response");

	if(res.error) {
		send_data(GET_FAILED_REPLY, strlen(GET_FAILED_REPLY));
		return;
	}

	if(!res.val) {
		send_data("END\r\n", 5);
		return;
	}

	struct iovec vb[6];
	vb[0].iov_base = const_cast<char*>("VALUE ");
	vb[0].iov_len  = 6;
	vb[1].iov_base = const_cast<char*>(res.key);
	vb[1].iov_len  = res.keylen;
	vb[2].iov_base = const_cast<char*>(" 0 ");
	vb[2].iov_len  = 3;
	char* numbuf = (char*)res.life->malloc(10+3);  // uint32 + \r\n\0
	vb[3].iov_base = numbuf;
	vb[3].iov_len  = sprintf(numbuf, "%u\r\n", res.vallen);
	vb[4].iov_base = const_cast<char*>(res.val);
	vb[4].iov_len  = res.vallen;
	vb[5].iov_base = const_cast<char*>("\r\nEND\r\n");
	vb[5].iov_len  = 7;
	send_datav(vb, 6, res.life);
}


void MemprotoText::Connection::ResMultiGet::response(get_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("get multi response ",m_count);

	mp::pthread_scoped_lock lk(m_mutex);
	--m_count;

	if(res.error || !res.val) {
		return;
	}

	struct iovec vb[6];
	vb[0].iov_base = const_cast<char*>("VALUE ");
	vb[0].iov_len  = 6;
	vb[1].iov_base = const_cast<char*>(res.key);
	vb[1].iov_len  = res.keylen;
	vb[2].iov_base = const_cast<char*>(" 0 ");
	vb[2].iov_len  = 3;
	char* numbuf = (char*)res.life->malloc(10+3);  // uint32 + \r\n\0
	vb[3].iov_base = numbuf;
	vb[3].iov_len  = sprintf(numbuf, "%u\r\n", res.vallen);
	vb[4].iov_base = const_cast<char*>(res.val);
	vb[4].iov_len  = res.vallen;
	if(m_count > 0) {
		vb[5].iov_base = const_cast<char*>("\r\n");
		vb[5].iov_len  = 2;
	} else {
		vb[5].iov_base = const_cast<char*>("\r\nEND\r\n");
		vb[5].iov_len  = 7;
	}
	send_datav(vb, 6, res.life);
}


void MemprotoText::Connection::ResSet::response(set_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("set response");

	if(res.error) {
		send_data(STORE_FAILED_REPLY, strlen(STORE_FAILED_REPLY));
		return;
	}

	send_data("STORED\r\n", 8);
}

void MemprotoText::Connection::ResSet::no_response(set_response& res)
{ }


void MemprotoText::Connection::ResDelete::response(delete_response& res)
{
	if(!is_valid()) { return; }
	LOG_TRACE("delete response");

	if(res.error) {
		send_data(DELETE_FAILED_REPLY, strlen(DELETE_FAILED_REPLY));
		return;
	}
	if(res.deleted) {
		send_data("DELETED\r\n", 9);
	} else {
		send_data("NOT FOUND\r\n", 11);
	}
}

void MemprotoText::Connection::ResDelete::no_response(delete_response& res)
{ }


}  // namespace kumo
