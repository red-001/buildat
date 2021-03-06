// http://www.apache.org/licenses/LICENSE-2.0
// Copyright 2014 Perttu Ahola <celeron55@gmail.com>
#pragma once
#include "core/types.h"

namespace interface
{
	struct TCPSocket
	{
		virtual ~TCPSocket(){}
		virtual int fd() const = 0;
		virtual bool good() const = 0;
		virtual void release_fd() = 0;
		virtual void close_fd() = 0;
		virtual bool listen_fd() = 0;
		virtual bool connect_fd(const ss_ &address, const ss_ &port) = 0;
		// Special values "any4", "any6" and "any"
		virtual bool bind_fd(const ss_ &address, const ss_ &port) = 0;
		virtual bool accept_fd(const TCPSocket &listener) = 0;
		virtual bool send_fd(const ss_ &data) = 0;
		virtual bool wait_data(int timeout_us) = 0;
		virtual ss_ get_local_address() const = 0;
		virtual ss_ get_remote_address() const = 0;
	};

	TCPSocket* createTCPSocket(int fd = -1);
}

// vim: set noet ts=4 sw=4:
