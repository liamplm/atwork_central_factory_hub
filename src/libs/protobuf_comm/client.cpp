
/***************************************************************************
 *  client.cpp - Protobuf stream protocol - client
 *
 *  Created: Thu Jan 31 17:38:04 2013
 *  Copyright  2013  Tim Niemueller [www.niemueller.de]
 ****************************************************************************/

/*  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the authors nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <protobuf_comm/client.h>

#include <boost/lexical_cast.hpp>

using namespace boost::asio;
using namespace boost::system;

namespace protobuf_comm {
#if 0 /* just to make Emacs auto-indent happy */
}
#endif

/** @class ProtobufStreamClient <protobuf_comm/client.h>
 * Stream client for protobuf message transmission.
 * The client opens a TCP connection (IPv4) to a specified server and
 * send and receives messages to the remote.
 * @author Tim Niemueller
 */

/** Constructor. */
ProtobufStreamClient::ProtobufStreamClient()
  : io_service_(), resolver_(io_service_), socket_(io_service_)
{
  connected_ = false;
  outbound_active_ = false;
  in_data_size_ = 1024;
  in_data_ = malloc(in_data_size_);
}


/** Destructor. */
ProtobufStreamClient::~ProtobufStreamClient()
{
  if (asio_thread_.joinable()) {
    disconnect();
    io_service_.stop();
    asio_thread_.join();
  }
  free(in_data_);
}


void
ProtobufStreamClient::io_service_run()
{
  std::lock_guard<std::mutex> lock(asio_mutex_);
  io_service_.run();
  io_service_.reset();
}

void
ProtobufStreamClient::run_asio()
{
  if (asio_mutex_.try_lock()) {
    // thread was running before
    if (asio_thread_.joinable()) {
      asio_thread_.join();
    }
    asio_mutex_.unlock();
    asio_thread_ = std::thread(&ProtobufStreamClient::io_service_run, this);
  }
}


/** Asynchronous connect.
 * This triggers connection establishment. The method does not block,
 * i.e. it returns immediately and does not wait for the connection to
 * be established.
 * @param host host to connect to
 * @param port TCP port to connect to
 */
void
ProtobufStreamClient::async_connect(const char *host, unsigned short port)
{
  ip::tcp::resolver::query query(host, boost::lexical_cast<std::string>(port));
  resolver_.async_resolve(query,
			  boost::bind(&ProtobufStreamClient::handle_resolve, this,
				      boost::asio::placeholders::error,
				      boost::asio::placeholders::iterator));

  run_asio();
}


void
ProtobufStreamClient::handle_resolve(const boost::system::error_code& err,
				     ip::tcp::resolver::iterator endpoint_iterator)
{
  if (! err) {
    // Attempt a connection to each endpoint in the list until we
    // successfully establish a connection.
    boost::asio::async_connect(socket_, endpoint_iterator,
			       boost::bind(&ProtobufStreamClient::handle_connect, this,
					   boost::asio::placeholders::error));
  } else {
    disconnect();
    sig_disconnected_(err);
  }
}

void
ProtobufStreamClient::handle_connect(const boost::system::error_code &err)
{
  if (! err) {
    connected_ = true;
    start_recv();
    sig_connected_();
  } else {
    disconnect();
    sig_disconnected_(err);
  }
}

void
ProtobufStreamClient::disconnect_nosig()
{
  boost::system::error_code err;
  socket_.shutdown(ip::tcp::socket::shutdown_both, err);
  socket_.close();
  connected_ = false;
}


/** Disconnect from remote host. */
void
ProtobufStreamClient::disconnect()
{
  disconnect_nosig();
  sig_disconnected_(boost::system::error_code());
}


void
ProtobufStreamClient::start_recv()
{
  boost::asio::async_read(socket_,
			  boost::asio::buffer(&in_frame_header_, sizeof(frame_header_t)),
			  boost::bind(&ProtobufStreamClient::handle_read_header,
				      this, boost::asio::placeholders::error));
}

void
ProtobufStreamClient::handle_read_header(const boost::system::error_code& error)
{
  if (! error) {
    size_t to_read = ntohl(in_frame_header_.payload_size);
    if (to_read > in_data_size_) {
      void *new_data = realloc(in_data_, to_read);
      if (new_data) {
	in_data_size_ = to_read;
	in_data_ = new_data;
      } else {
	sig_disconnected_(errc::make_error_code(errc::not_enough_memory));
      }
    }
    // setup new read
    boost::asio::async_read(socket_,
			    boost::asio::buffer(in_data_, to_read),
			    boost::bind(&ProtobufStreamClient::handle_read_message,
					this, boost::asio::placeholders::error));
  }
}

void
ProtobufStreamClient::handle_read_message(const boost::system::error_code& error)
{
  if (! error) {
    std::shared_ptr<google::protobuf::Message> m =
      message_register_.deserialize(in_frame_header_, in_data_);
    uint16_t comp_id   = ntohs(in_frame_header_.component_id);
    uint16_t msg_type  = ntohs(in_frame_header_.msg_type);
    sig_rcvd_(comp_id, msg_type, m);

    start_recv();
  }
}

void
ProtobufStreamClient::handle_write(const boost::system::error_code& error,
				   size_t /*bytes_transferred*/,
				   QueueEntry *entry)
{
  delete entry;

  if (! error) {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    if (! outbound_queue_.empty()) {
      QueueEntry *entry = outbound_queue_.front();
      outbound_queue_.pop();
      boost::asio::async_write(socket_, entry->buffers,
			       boost::bind(&ProtobufStreamClient::handle_write, this,
					   boost::asio::placeholders::error,
					   boost::asio::placeholders::bytes_transferred,
					   entry));
    } else {
      outbound_active_ = false;
    }
  } else {
    sig_disconnected_(error);
  }
}


/** Send a message to the server.
 * @param component_id ID of the component to address
 * @param msg_type numeric message type
 * @param m message to send
 */
void
ProtobufStreamClient::send(uint16_t component_id, uint16_t msg_type,
			   google::protobuf::Message &m)
{
  QueueEntry *entry = new QueueEntry();
  message_register_.serialize(component_id, msg_type, m,
			      entry->frame_header, entry->serialized_message);

  entry->buffers[0] = boost::asio::buffer(&entry->frame_header, sizeof(frame_header_t));
  entry->buffers[1] = boost::asio::buffer(entry->serialized_message);
 
  std::lock_guard<std::mutex> lock(outbound_mutex_);
  if (outbound_active_) {
    outbound_queue_.push(entry);
  } else {
    outbound_active_ = true;
    boost::asio::async_write(socket_, entry->buffers,
			     boost::bind(&ProtobufStreamClient::handle_write, this,
					 boost::asio::placeholders::error,
					 boost::asio::placeholders::bytes_transferred,
					 entry));
    run_asio();
  }
}

} // end namespace protobuf_comm