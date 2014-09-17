#include "interface/tcpsocket.h"
#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h> // strerror()
#include <netinet/in.h>
#include <iomanip>
#include <netdb.h>

namespace interface {

const unsigned char prefix[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xFF, 0xFF};

bool sockaddr_to_bytes(const sockaddr_storage* ptr, sv_<uchar>& to)
{
	if(ptr->ss_family == AF_INET)
	{
		uchar* u = (uchar*)&((struct sockaddr_in*)ptr)->sin_addr.s_addr;
		to.assign(u, u + 4);
		return true;
	}
	else if(ptr->ss_family == AF_INET6)
	{
		uchar* u = (uchar*)&((struct sockaddr_in6*)ptr)->sin6_addr.s6_addr;
		if(memcmp(prefix, u, sizeof(prefix)) == 0){
			to.assign(u + 12, u + 16);
			return true;
		}
		to.assign(u, u + 16);
		return true;
	}

	return false;
};

std::string address_bytes_to_string(const sv_<uchar> &ip)
{
	std::ostringstream os;
	for(size_t i=0; i<ip.size(); i++){
		if(ip.size() == 4){
			os<<std::dec<<std::setfill('0')<<std::setw(0)
					<<((uint32_t)ip[i]&0xff);
			if(i < ip.size()-1)
				os<<".";
		} else {
			os<<std::hex<<std::setfill('0')<<std::setw(2)
					<<((uint32_t)ip[i]&0xff);
			i++;
			if(i < ip.size())
				os<<std::hex<<std::setfill('0')<<std::setw(2)
						<<((uint32_t)ip[i]&0xff);
			if(i < ip.size()-1)
				os<<":";
		}
	}
	return os.str();
}


struct CTCPSocket: public TCPSocket
{
	int m_fd = 0;

	int fd() const
	{
		return m_fd;
	}
	bool good() const
	{
		return (m_fd != -1);
	}
	void close_fd()
	{
		if(m_fd != -1)
			close(m_fd);
		m_fd = -1;
	}
	bool listen_fd()
	{
		if(m_fd == -1)
			return false;
		if(listen(m_fd, 5) == -1){
			std::cerr<<"TCPSocket::listen_fd(): "<<strerror(errno)<<std::endl;
			return false;
		}
		return true;
	}
	bool connect_fd(const ss_ &address, const ss_ &port)
	{
		close_fd();

		struct addrinfo hints;
		struct addrinfo *res0 = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		if(address == "any")
			hints.ai_flags = AI_PASSIVE; // Wildcard address
		const char *address_c = (address == "any" ? NULL : address.c_str());
		const char *port_c = (port == "any" ? NULL : port.c_str());
		int err = getaddrinfo(address_c, port_c, &hints, &res0);
		if(err){
			std::cerr<<"getaddrinfo: "<<gai_strerror(err)<<std::endl;
			return false;
		}
		if(res0 == NULL){
			std::cerr<<"getaddrinfo: No results"<<std::endl;
			return false;
		}

		// Try to use one of the results
		int fd = -1;
		int i=0;
		for(struct addrinfo *res = res0; res != NULL; res = res->ai_next, i++)
		{
			std::cerr<<"Trying addrinfo #"<<i<<std::endl;
			int try_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if(try_fd == -1){
				std::cerr<<"socket: "<<strerror(errno)<<std::endl;
				continue;
			}
			if(connect(try_fd, res->ai_addr, res->ai_addrlen) == -1){
				std::cerr<<"connect: "<<strerror(errno)<<std::endl;
				close(try_fd);
				continue;
			}
			fd = try_fd;
			break;
		}
		freeaddrinfo(res0);

		if(fd == -1){
			std::cerr<<"Failed to create and connect socket"<<std::endl;
			return false;
		}

		int val = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

		// Set this so that forked child processes don't prevent re-opening the
		// same port after crash
		if(fcntl(fd, F_SETFD, FD_CLOEXEC) != 0){
			std::cerr<<"Failed to set socket FD_CLOEXEC"<<std::endl;
			return false;
		}

		m_fd = fd;
		return true;
	}
	bool bind_fd(const ss_ &address, const ss_ &port)
	{
		close_fd();

		struct addrinfo hints;
		struct addrinfo *res0 = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		std::string address1 = address;
		if(address1 == "any4"){
			address1 = "any";
			hints.ai_family = AF_INET;
		}
		if(address1 == "any6"){
			address1 = "any";
			hints.ai_family = AF_INET6;
		}
		if(address1 == "any"){
			hints.ai_flags = AI_PASSIVE; // Wildcard address
		}
		const char *address_c = (address1 == "any" ? NULL : address1.c_str());
		const char *port_c = (port == "any" ? NULL : port.c_str());
		int err = getaddrinfo(address_c, port_c, &hints, &res0);
		if(err){
			std::cerr<<"getaddrinfo: "<<gai_strerror(err)<<std::endl;
			return false;
		}
		if(res0 == NULL){
			std::cerr<<"getaddrinfo: No results"<<std::endl;
			return false;
		}

		// Try to use one of the results
		int fd = -1;
		int i=0;
		for(struct addrinfo *res = res0; res != NULL; res = res->ai_next, i++)
		{
			//std::cerr<<"Trying addrinfo #"<<i<<std::endl;
			int try_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if(try_fd == -1){
				//std::cerr<<"socket: "<<strerror(errno)<<std::endl;
				continue;
			}
			int val = 1;
			setsockopt(try_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
			if(res->ai_family == AF_INET6){
				int val = 1;
				setsockopt(try_fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
			}
			if(bind(try_fd, res->ai_addr, res->ai_addrlen) == -1){
				//std::cerr<<"bind: "<<strerror(errno)<<std::endl;
				close(try_fd);
				continue;
			}
			fd = try_fd;
			break;
		}
		freeaddrinfo(res0);

		if(fd == -1){
			std::cerr<<"Failed to create and bind socket"<<std::endl;
			return false;
		}

		// Set this so that forked child processes don't prevent re-opening the
		// same port after crash
		if(fcntl(fd, F_SETFD, FD_CLOEXEC) != 0){
			std::cerr<<"Failed to set socket FD_CLOEXEC"<<std::endl;
			return false;
		}

		m_fd = fd;
		return true;
	}	
	bool accept_fd(const TCPSocket &listener)
	{
		close_fd();

		if(!listener.good())
			return false;

		struct sockaddr_storage pin;
		socklen_t pin_len = sizeof(pin);
		int fd_client = accept(listener.fd(), (struct sockaddr *)  &pin, &pin_len);
		if(fd_client == -1){
			std::cerr<<"accept: "<<strerror(errno)<<std::endl;
			return false;
		}

		int val = 1;
		setsockopt(fd_client, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

		m_fd = fd_client;
		return true;
	}
	bool send_fd(const ss_ &data)
	{
		if(m_fd == -1)
			return false;
		if(send(m_fd, &data[0], data.size(), 0) == -1){
			std::cerr<<"send: "<<strerror(errno)<<std::endl;
			return false;
		}
		return true;
	}
	ss_ get_local_address() const
	{
		if(m_fd == -1)
			return "";
		struct sockaddr_storage sa;
		socklen_t sa_len = sizeof(sa);
		if(getpeername(m_fd, (sockaddr*)&sa, &sa_len) == -1)
			return "";
		sv_<uchar> a;
		if(!sockaddr_to_bytes(&sa, a))
			return "";
		return address_bytes_to_string(a);
	}
	ss_ get_remote_address() const
	{
		if(m_fd == -1)
			return "";
		struct sockaddr_storage sa;
		socklen_t sa_len = sizeof(sa);
		if(getsockname(m_fd, (sockaddr*)&sa, &sa_len) == -1)
			return "";
		sv_<uchar> a;
		if(!sockaddr_to_bytes(&sa, a))
			return "";
		return address_bytes_to_string(a);
	}
};

TCPSocket* createTCPSocket()
{
	return new CTCPSocket();
}

}