/**
 *  Copyright 2009-2012 10gen, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#ifndef WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include "types.h"


/* Wait on socket availability with a timeout
 * TODO: Port to use poll() instead of select().
 *
 * Returns:
 * 0 on success
 * -1 on failure, but not critical enough to throw an exception
 * 1.. on failure, and throw an exception. The return value is the error code
 */
int mongo_io_wait_with_timeout(int sock, int to, char **error_message)
{
	/* No socket timeout.. But we default to 1 second for historical reasons */
	if (to < 1) {
		to = 10000;
	}
	while (1) {
		int status;
		struct timeval timeout;
		fd_set readfds, exceptfds;

		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		FD_ZERO(&exceptfds);
		FD_SET(sock, &exceptfds);

		timeout.tv_sec = to / 1000 ;
		timeout.tv_usec = (to % 1000) * 1000;

		status = select(sock+1, &readfds, NULL, &exceptfds, &timeout);

		if (status == -1) {
			/* on EINTR, retry - this resets the timeout however to its full length */
			if (errno == EINTR) {
				continue;
			}

			*error_message = strdup(strerror(errno));
			return 13;
		}

		if (FD_ISSET(sock, &exceptfds)) {
			*error_message = strdup("Exceptional condition on socket");
			return 17;
		}

		if (status == 0 && !FD_ISSET(sock, &readfds)) {
			*error_message = malloc(256);
			snprintf(
				*error_message, 256,
				"cursor timed out (timeout: %d, time left: %ld:%ld, status: %d)",
				to, (long) timeout.tv_sec, (long) timeout.tv_usec, status
			);
			return 80;
		}

		/* if our descriptor is ready break out */
		if (FD_ISSET(sock, &readfds)) {
			break;
		}
	}

	return 0;
}
/*
 * Low-level send function.
 *
 * Goes through the buffer sending 4K byte batches.
 * On failure, sets errmsg to errno string and returns -1.
 * On success, returns number of bytes sent.
 * Does not attempt to reconnect nor throw any exceptions.
 *
 * On failure, the calling function is responsible for disconnecting
 */
int mongo_io_send(int sock, char *packet, int total, char **error_message)
{
	int sent = 0, status = 1;

	while (sent < total && status > 0) {
		int len = 4096 < (total - sent) ? 4096 : total - sent;

		status = send(sock, (const char*)packet + sent, len, 0);

		if (status == -1) {
			*error_message = strdup(strerror(errno));
			return -1;
		}
		sent += status;
	}

	return sent;
}

/*
 * Low-level receive functions.
 *
 * On failure, sets errmsg to errno string and returns -1.
 * On success, returns number of bytes read.
 * Does not attempt to reconnect nor throw any exceptions.
 *
 * On failure, the calling function is responsible for disconnecting
 */
int mongo_io_recv_header(int sock, mongo_server_options *options, char *reply_buffer, int size, char **error_message)
{
	int status = mongo_io_wait_with_timeout(sock, options->socketTimeoutMS, error_message);

	if (status != 0) {
		/* We don't care which failure it was, it just failed and the error_message has been set */
		return -1;
	}
	status = recv(sock, reply_buffer, size, 0);

	if (status == -1) {
		*error_message = strdup(strerror(errno));
		return -1;
	} else if (status == 0) {
		*error_message = strdup("The socket is closed");
		return -1;
	}
	return status;
}

int mongo_io_recv_data(int sock, mongo_server_options *options, void *dest, int size, char **error_message)
{
	int num = 1, received = 0;

	// this can return FAILED if there is just no more data from db
	while (received < size && num > 0) {
		int len = 4096 < (size - received) ? 4096 : size - received;

		if (mongo_io_wait_with_timeout(sock, options->socketTimeoutMS, error_message) != 0) {
			/* We don't care which failure it was, it just failed */
			return 0;
		}
		// windows gives a WSAEFAULT if you try to get more bytes
		num = recv(sock, (char*)dest, len, 0);

		if (num < 0) {
			return 0;
		}

		dest = (char*)dest + num;
		received += num;
	}
	return received;
}

