/**
 * @brief FTP plugin
 * @file ftp.cpp
 * @author Vladimir Ermakov <vooon341@gmail.com>
 *
 * @addtogroup plugin
 * @{
 */
/*
 * Copyright 2014,2016 Vladimir Ermakov.
 *
 * This file is part of the mavros package and subject to the license terms
 * in the top-level LICENSE file of the mavros repository.
 * https://github.com/mavlink/mavros/tree/master/LICENSE.md
 */

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <mavros/mavros_plugin.h>

#include <std_srvs/srv/empty.hpp>
#include <mavros_msgs/msg/file_entry.hpp>
#include <mavros_msgs/srv/file_list.hpp>
#include <mavros_msgs/srv/file_open.hpp>
#include <mavros_msgs/srv/file_close.hpp>
#include <mavros_msgs/srv/file_read.hpp>
#include <mavros_msgs/srv/file_write.hpp>
#include <mavros_msgs/srv/file_remove.hpp>
#include <mavros_msgs/srv/file_make_dir.hpp>
#include <mavros_msgs/srv/file_remove_dir.hpp>
#include <mavros_msgs/srv/file_truncate.hpp>
#include <mavros_msgs/srv/file_rename.hpp>
#include <mavros_msgs/srv/file_checksum.hpp>

// enable debugging messages
//#define FTP_LL_DEBUG

#define ROS_ASSERT(cond) assert(cond)
#define ROS_ASSERT_MSG(cond, msg) assert(cond && msg)

namespace mavros {
namespace std_plugins {
using utils::enum_value;

/**
 * @brief FTP Request message abstraction class
 *
 * @note This class not portable, and works on little-endian machines only.
 */
class FTPRequest : public mavlink::common::msg::FILE_TRANSFER_PROTOCOL {
public:
	/// @brief This is the payload which is in mavlink_file_transfer_protocol_t.payload.
	/// We pad the structure ourselves to 32 bit alignment to avoid usage of any pack pragmas.
	struct PayloadHeader {
		uint16_t	seqNumber;	///< sequence number for message
		uint8_t		session;	///< Session id for read and write commands
		uint8_t		opcode;		///< Command opcode
		uint8_t		size;		///< Size of data
		uint8_t		req_opcode;	///< Request opcode returned in kRspAck, kRspNak message
		uint8_t		padding[2];	///< 32 bit aligment padding
		uint32_t	offset;		///< Offsets for List and Read commands
		uint8_t		data[];		///< command data, varies by Opcode
	};

	/// @brief Command opcodes
	enum Opcode : uint8_t {
		kCmdNone,		///< ignored, always acked
		kCmdTerminateSession,	///< Terminates open Read session
		kCmdResetSessions,	///< Terminates all open Read sessions
		kCmdListDirectory,	///< List files in <path> from <offset>
		kCmdOpenFileRO,		///< Opens file at <path> for reading, returns <session>
		kCmdReadFile,		///< Reads <size> bytes from <offset> in <session>
		kCmdCreateFile,		///< Creates file at <path> for writing, returns <session>
		kCmdWriteFile,		///< Writes <size> bytes to <offset> in <session>
		kCmdRemoveFile,		///< Remove file at <path>
		kCmdCreateDirectory,	///< Creates directory at <path>
		kCmdRemoveDirectory,	///< Removes Directory at <path>, must be empty
		kCmdOpenFileWO,		///< Opens file at <path> for writing, returns <session>
		kCmdTruncateFile,	///< Truncate file at <path> to <offset> length
		kCmdRename,		///< Rename <path1> to <path2>
		kCmdCalcFileCRC32,	///< Calculate CRC32 for file at <path>
		kCmdBurstReadFile,	///< Burst download session file

		kRspAck = 128,		///< Ack response
		kRspNak			///< Nak response
	};

	/// @brief Error codes returned in Nak response.
	enum ErrorCode : uint8_t {
		kErrNone,
		kErrFail,			///< Unknown failure
		kErrFailErrno,			///< Command failed, errno sent back in PayloadHeader.data[1]
		kErrInvalidDataSize,		///< PayloadHeader.size is invalid
		kErrInvalidSession,		///< Session is not currently open
		kErrNoSessionsAvailable,	///< All available Sessions in use
		kErrEOF,			///< Offset past end of file for List and Read commands
		kErrUnknownCommand,		///< Unknown command opcode
		kErrFailFileExists,		///< File exists already
		kErrFailFileProtected		///< File is write protected
	};

	static const char	DIRENT_FILE = 'F';
	static const char	DIRENT_DIR = 'D';
	static const char	DIRENT_SKIP = 'S';
	//! payload.size() - header bytes
	static const uint8_t	DATA_MAXSZ = 251 - sizeof(PayloadHeader);

	uint8_t *raw_payload() {
		return payload.data();
	}

	inline PayloadHeader *header() {
		return reinterpret_cast<PayloadHeader *>(payload.data());
	}

	uint8_t *data() {
		return header()->data;
	}

	char *data_c() {
		return reinterpret_cast<char *>(header()->data);
	}

	uint32_t *data_u32() {
		return reinterpret_cast<uint32_t *>(header()->data);
	}

	/**
	 * @brief Copy string to payload
	 *
	 * @param[in] s  payload string
	 * @note this function allow null termination inside string
	 *       it used to send multiple strings in one message
	 */
	void set_data_string(std::string &s)
	{
		size_t sz = (s.size() < DATA_MAXSZ - 1) ? s.size() : DATA_MAXSZ - 1;

		memcpy(data_c(), s.c_str(), sz);
		data_c()[sz] = '\0';
		header()->size = sz;
	}

	uint8_t get_target_system_id() {
		return target_system;
	}

	/**
	 * @brief Decode and check target system
	 */
	bool decode_valid(UAS *uas)
	{
#ifdef FTP_LL_DEBUG
		auto hdr = header();
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:rm: SEQ(%u) SESS(%u) OPCODE(%u) RQOP(%u) SZ(%u) OFF(%u)",
				hdr->seqNumber, hdr->session, hdr->opcode, hdr->req_opcode, hdr->size, hdr->offset);
#endif

		return UAS_FCU(uas)->get_system_id() == target_system;
	}

	/**
	 * @brief Encode and send message
	 */
	void send(UAS *uas, uint16_t seqNumber)
	{
		target_network = 0;
		target_system = uas->get_tgt_system();
		target_component = uas->get_tgt_component();

		auto hdr = header();
		hdr->seqNumber = seqNumber;
		hdr->req_opcode = kCmdNone;

#ifdef FTP_LL_DEBUG
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:sm: SEQ(%u) SESS(%u) OPCODE(%u) SZ(%u) OFF(%u)",
				hdr->seqNumber, hdr->session, hdr->opcode, hdr->size, hdr->offset);
#endif

		UAS_FCU(uas)->send_message_ignore_drop(*this);
	}

	FTPRequest() :
		mavlink::common::msg::FILE_TRANSFER_PROTOCOL{}
	{ }

	explicit FTPRequest(Opcode op, uint8_t session = 0) :
		mavlink::common::msg::FILE_TRANSFER_PROTOCOL{}
	{
		header()->session = session;
		header()->opcode = op;
	}
};


/**
 * @brief FTP plugin.
 */
class FTPPlugin : public plugin::PluginBase {
public:
	FTPPlugin() : PluginBase(),
		op_state(OP::IDLE),
		last_send_seqnr(0),
		active_session(0),
		is_error(false),
		r_errno(0),
		list_offset(0),
		read_offset(0),
		write_offset(0),
		open_size(0),
		read_size(0),
		read_buffer {},
		checksum_crc32(0)
	{ }

	void initialize(UAS &uas_)
	{
		PluginBase::initialize(uas_);
		ftp_nh = uas_.mavros_node->create_sub_node("ftp");

		// since C++ generator do not produce field length defs make check explicit.
		FTPRequest r;
		if (!(r.payload.size() - sizeof(FTPRequest::PayloadHeader) == r.DATA_MAXSZ)) {
			RCUTILS_LOG_FATAL("FTP bad data size");
		}

		list_srv = ftp_nh->create_service<mavros_msgs::srv::FileList>("list", std::bind(&FTPPlugin::list_cb, this, std::placeholders::_1, std::placeholders::_2));
		open_srv = ftp_nh->create_service<mavros_msgs::srv::FileOpen>("open", std::bind(&FTPPlugin::open_cb, this, std::placeholders::_1, std::placeholders::_2));
		close_srv = ftp_nh->create_service<mavros_msgs::srv::FileClose>("close", std::bind(&FTPPlugin::close_cb, this, std::placeholders::_1, std::placeholders::_2));
		read_srv = ftp_nh->create_service<mavros_msgs::srv::FileRead>("read", std::bind(&FTPPlugin::read_cb, this, std::placeholders::_1, std::placeholders::_2));
		write_srv = ftp_nh->create_service<mavros_msgs::srv::FileWrite>("write", std::bind(&FTPPlugin::write_cb, this, std::placeholders::_1, std::placeholders::_2));
		mkdir_srv = ftp_nh->create_service<mavros_msgs::srv::FileMakeDir>("mkdir", std::bind(&FTPPlugin::mkdir_cb, this, std::placeholders::_1, std::placeholders::_2));
		rmdir_srv = ftp_nh->create_service<mavros_msgs::srv::FileRemoveDir>("rmdir", std::bind(&FTPPlugin::rmdir_cb, this, std::placeholders::_1, std::placeholders::_2));
		remove_srv = ftp_nh->create_service<mavros_msgs::srv::FileRemove>("remove", std::bind(&FTPPlugin::remove_cb, this, std::placeholders::_1, std::placeholders::_2));
		truncate_srv = ftp_nh->create_service<mavros_msgs::srv::FileTruncate>("truncate", std::bind(&FTPPlugin::truncate_cb, this, std::placeholders::_1, std::placeholders::_2));
		reset_srv = ftp_nh->create_service<std_srvs::srv::Empty>("reset", std::bind(&FTPPlugin::reset_cb, this, std::placeholders::_1, std::placeholders::_2));
		rename_srv = ftp_nh->create_service<mavros_msgs::srv::FileRename>("rename", std::bind(&FTPPlugin::rename_cb, this, std::placeholders::_1, std::placeholders::_2));
		checksum_srv = ftp_nh->create_service<mavros_msgs::srv::FileChecksum>("checksum", std::bind(&FTPPlugin::checksum_cb, this, std::placeholders::_1, std::placeholders::_2));
	}

	Subscriptions get_subscriptions()
	{
		return {
			make_handler(&FTPPlugin::handle_file_transfer_protocol),
		};
	}

private:
	rclcpp::Node::SharedPtr ftp_nh;
	rclcpp::Service<mavros_msgs::srv::FileList>::SharedPtr list_srv;
	rclcpp::Service<mavros_msgs::srv::FileOpen>::SharedPtr open_srv;
	rclcpp::Service<mavros_msgs::srv::FileClose>::SharedPtr close_srv;
	rclcpp::Service<mavros_msgs::srv::FileRead>::SharedPtr read_srv;
	rclcpp::Service<mavros_msgs::srv::FileWrite>::SharedPtr write_srv;
	rclcpp::Service<mavros_msgs::srv::FileMakeDir>::SharedPtr mkdir_srv;
	rclcpp::Service<mavros_msgs::srv::FileRemoveDir>::SharedPtr rmdir_srv;
	rclcpp::Service<mavros_msgs::srv::FileRemove>::SharedPtr remove_srv;
	rclcpp::Service<mavros_msgs::srv::FileRename>::SharedPtr rename_srv;
	rclcpp::Service<mavros_msgs::srv::FileTruncate>::SharedPtr truncate_srv;
	rclcpp::Service<std_srvs::srv::Empty>::SharedPtr reset_srv;
	rclcpp::Service<mavros_msgs::srv::FileChecksum>::SharedPtr checksum_srv;

	//! This type used in servicies to store 'data' fileds.
	typedef std::vector<uint8_t> V_FileData;

	enum class OP {
		IDLE,
		ACK,
		LIST,
		OPEN,
		READ,
		WRITE,
		CHECKSUM
	};

	OP op_state;
	uint16_t last_send_seqnr;	//!< seqNumber for send.
	uint32_t active_session;	//!< session id of current operation

	std::mutex cond_mutex;
	std::condition_variable cond;	//!< wait condvar
	bool is_error;			//!< error signaling flag (timeout/proto error)
	int r_errno;			//!< store errno from server

	// FTP:List
	uint32_t list_offset;
	std::string list_path;
	std::vector<mavros_msgs::msg::FileEntry> list_entries;

	// FTP:Open / FTP:Close
	std::string open_path;
	size_t open_size;
	std::map<std::string, uint32_t> session_file_map;

	// FTP:Read
	size_t read_size;
	uint32_t read_offset;
	V_FileData read_buffer;

	// FTP:Write
	uint32_t write_offset;
	V_FileData write_buffer;
	V_FileData::iterator write_it;

	// FTP:CalcCRC32
	uint32_t checksum_crc32;

	// Timeouts,
	// computed as x4 time that needed for transmission of
	// one message at 57600 baud rate
	static constexpr int LIST_TIMEOUT_MS = 5000;
	static constexpr int OPEN_TIMEOUT_MS = 200;
	static constexpr int CHUNK_TIMEOUT_MS = 200;

	//! Maximum difference between allocated space and used
	static constexpr size_t MAX_RESERVE_DIFF = 0x10000;

	//! @todo exchange speed calculation
	//! @todo diagnostics
	//! @todo multisession not present anymore

	/* -*- message handler -*- */

	//! handler for mavlink::common::msg::FILE_TRANSFER_PROTOCOL
	void handle_file_transfer_protocol(const mavlink::mavlink_message_t *msg, FTPRequest &req)
	{
		if (!req.decode_valid(m_uas)) {
			RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP: Wrong System Id, MY %u, TGT %u",
					UAS_FCU(m_uas)->get_system_id(), req.get_target_system_id());
			return;
		}

		const uint16_t incoming_seqnr = req.header()->seqNumber;
		const uint16_t expected_seqnr = last_send_seqnr + 1;
		if (incoming_seqnr != expected_seqnr) {
			RCUTILS_LOG_WARN_NAMED("ftp", "FTP: Lost sync! seqnr: %u != %u",
					incoming_seqnr, expected_seqnr);
			go_idle(true, EILSEQ);
			return;
		}

		last_send_seqnr = incoming_seqnr;

		// logic from QGCUASFileManager.cc
		if (req.header()->opcode == FTPRequest::kRspAck)
			handle_req_ack(req);
		else if (req.header()->opcode == FTPRequest::kRspNak)
			handle_req_nack(req);
		else {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Unknown request response: %u", req.header()->opcode);
			go_idle(true, EBADRQC);
		}
	}

	void handle_req_ack(FTPRequest &req)
	{
		switch (op_state) {
		case OP::IDLE:		send_reset();			break;
		case OP::ACK:		go_idle(false);			break;
		case OP::LIST:		handle_ack_list(req);		break;
		case OP::OPEN:		handle_ack_open(req);		break;
		case OP::READ:		handle_ack_read(req);		break;
		case OP::WRITE:		handle_ack_write(req);		break;
		case OP::CHECKSUM:	handle_ack_checksum(req);	break;
		default:
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: wrong op_state");
			go_idle(true, EBADRQC);
		}
	}

	void handle_req_nack(FTPRequest &req)
	{
		auto hdr = req.header();
		auto error_code = static_cast<FTPRequest::ErrorCode>(req.data()[0]);
		auto prev_op = op_state;

		ROS_ASSERT(hdr->size == 1 || (error_code == FTPRequest::kErrFailErrno && hdr->size == 2));

		op_state = OP::IDLE;
		if (error_code == FTPRequest::kErrFailErrno)
			r_errno = req.data()[1];
		// translate other protocol errors to errno
		else if (error_code == FTPRequest::kErrFail)
			r_errno = EFAULT;
		else if (error_code == FTPRequest::kErrInvalidDataSize)
			r_errno = EMSGSIZE;
		else if (error_code == FTPRequest::kErrInvalidSession)
			r_errno = EBADFD;
		else if (error_code == FTPRequest::kErrNoSessionsAvailable)
			r_errno = EMFILE;
		else if (error_code == FTPRequest::kErrUnknownCommand)
			r_errno = ENOSYS;

		if (prev_op == OP::LIST && error_code == FTPRequest::kErrEOF) {
			/* dir list done */
			list_directory_end();
			return;
		}
		else if (prev_op == OP::READ && error_code == FTPRequest::kErrEOF) {
			/* read done */
			read_file_end();
			return;
		}

		RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: NAK: %u Opcode: %u State: %u Errno: %d (%s)",
				error_code, hdr->req_opcode, enum_value(prev_op), r_errno, strerror(r_errno));
		go_idle(true);
	}

	void handle_ack_list(FTPRequest &req)
	{
		auto hdr = req.header();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: ACK List SZ(%u) OFF(%u)", hdr->size, hdr->offset);
		if (hdr->offset != list_offset) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Wrong list offset, req %u, ret %u",
					list_offset, hdr->offset);
			go_idle(true, EBADE);
			return;
		}

		uint8_t off = 0;
		uint32_t n_list_entries = 0;

		while (off < hdr->size) {
			const char *ptr = req.data_c() + off;
			const size_t bytes_left = hdr->size - off;

			size_t slen = strnlen(ptr, bytes_left);
			if ((ptr[0] == FTPRequest::DIRENT_SKIP && slen > 1) ||
					(ptr[0] != FTPRequest::DIRENT_SKIP && slen < 2)) {
				RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Incorrect list entry: %s", ptr);
				go_idle(true, ERANGE);
				return;
			}
			else if (slen == bytes_left) {
				RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Missing NULL termination in list entry");
				go_idle(true, EOVERFLOW);
				return;
			}

			if (ptr[0] == FTPRequest::DIRENT_FILE ||
					ptr[0] == FTPRequest::DIRENT_DIR) {
				add_dirent(ptr, slen);
			}
			else if (ptr[0] == FTPRequest::DIRENT_SKIP) {
				// do nothing
			}
			else {
				RCUTILS_LOG_WARN_NAMED("ftp", "FTP: Unknown list entry: %s", ptr);
			}

			off += slen + 1;
			n_list_entries++;
		}

		if (hdr->size == 0) {
			// dir empty, we are done
			list_directory_end();
		}
		else {
			ROS_ASSERT_MSG(n_list_entries > 0, "FTP:List don't parse entries");
			// Possibly more to come, try get more
			list_offset += n_list_entries;
			send_list_command();
		}
	}

	void handle_ack_open(FTPRequest &req)
	{
		auto hdr = req.header();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: ACK Open OPCODE(%u)", hdr->req_opcode);
		ROS_ASSERT(hdr->size == sizeof(uint32_t));
		open_size = *req.data_u32();

		RCUTILS_LOG_INFO_NAMED("ftp", "FTP:Open %s: success, session %u, size %zu",
				open_path.c_str(), hdr->session, open_size);
		session_file_map.insert(std::make_pair(open_path, hdr->session));
		go_idle(false);
	}

	void handle_ack_read(FTPRequest &req)
	{
		auto hdr = req.header();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: ACK Read SZ(%u)", hdr->size);
		if (hdr->session != active_session) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Read unexpected session");
			go_idle(true, EBADSLT);
			return;
		}

		if (hdr->offset != read_offset) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Read different offset");
			go_idle(true, EBADE);
			return;
		}

		// kCmdReadFile return cunks of DATA_MAXSZ or smaller (last chunk)
		// We requested specific amount of data, that can be smaller,
		// but not larger.
		const size_t bytes_left = read_size - read_buffer.size();
		const size_t bytes_to_copy = std::min<size_t>(bytes_left, hdr->size);

		read_buffer.insert(read_buffer.end(), req.data(), req.data() + bytes_to_copy);

		if (bytes_to_copy == FTPRequest::DATA_MAXSZ) {
			// Possibly more data
			read_offset += bytes_to_copy;
			send_read_command();
		}
		else
			read_file_end();
	}

	void handle_ack_write(FTPRequest &req)
	{
		auto hdr = req.header();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: ACK Write SZ(%u)", hdr->size);
		if (hdr->session != active_session) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Write unexpected session");
			go_idle(true, EBADSLT);
			return;
		}

		if (hdr->offset != write_offset) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Write different offset");
			go_idle(true, EBADE);
			return;
		}

		ROS_ASSERT(hdr->size == sizeof(uint32_t));
		const size_t bytes_written = *req.data_u32();

		// check that reported size not out of range
		const size_t bytes_left_before_advance = std::distance(write_it, write_buffer.end());
		ROS_ASSERT_MSG(bytes_written <= bytes_left_before_advance, "Bad write size");
		ROS_ASSERT(bytes_written != 0);

		// move iterator to written size
		std::advance(write_it, bytes_written);

		const size_t bytes_to_copy = write_bytes_to_copy();
		if (bytes_to_copy > 0) {
			// More data to write
			write_offset += bytes_written;
			send_write_command(bytes_to_copy);
		}
		else
			write_file_end();
	}

	void handle_ack_checksum(FTPRequest &req)
	{
		auto hdr = req.header();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: ACK CalcFileCRC32 OPCODE(%u)", hdr->req_opcode);
		ROS_ASSERT(hdr->size == sizeof(uint32_t));
		checksum_crc32 = *req.data_u32();

		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:Checksum: success, crc32: 0x%08x", checksum_crc32);
		go_idle(false);
	}

	/* -*- send helpers -*- */

	/**
	 * @brief Go to IDLE mode
	 *
	 * @param is_error_ mark that caused in error case
	 * @param r_errno_ set r_errno in error case
	 */
	void go_idle(bool is_error_, int r_errno_ = 0)
	{
		op_state = OP::IDLE;
		is_error = is_error_;
		if (is_error && r_errno_ != 0) r_errno = r_errno_;
		else if (!is_error) r_errno = 0;
		cond.notify_all();
	}

	void send_reset()
	{
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: kCmdResetSessions");
		if (!session_file_map.empty()) {
			RCUTILS_LOG_WARN_NAMED("ftp", "FTP: Reset closes %zu sessons",
					session_file_map.size());
			session_file_map.clear();
		}

		op_state = OP::ACK;
		FTPRequest req(FTPRequest::kCmdResetSessions);
		req.send(m_uas, last_send_seqnr);
	}

	/// Send any command with string payload (usually file/dir path)
	inline void send_any_path_command(FTPRequest::Opcode op, const std::string &debug_msg, std::string &path, uint32_t offset)
	{
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: %s%s off: %u", debug_msg.c_str(), path.c_str(), offset);
		FTPRequest req(op);
		req.header()->offset = offset;
		req.set_data_string(path);
		req.send(m_uas, last_send_seqnr);
	}

	void send_list_command() {
		send_any_path_command(FTPRequest::kCmdListDirectory, "kCmdListDirectory: ", list_path, list_offset);
	}

	void send_open_ro_command() {
		send_any_path_command(FTPRequest::kCmdOpenFileRO, "kCmdOpenFileRO: ", open_path, 0);
	}

	void send_open_wo_command() {
		send_any_path_command(FTPRequest::kCmdOpenFileWO, "kCmdOpenFileWO: ", open_path, 0);
	}

	void send_create_command() {
		send_any_path_command(FTPRequest::kCmdCreateFile, "kCmdCreateFile: ", open_path, 0);
	}

	void send_terminate_command(uint32_t session)
	{
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: kCmdTerminateSession: %u", session);
		FTPRequest req(FTPRequest::kCmdTerminateSession, session);
		req.header()->offset = 0;
		req.header()->size = 0;
		req.send(m_uas, last_send_seqnr);
	}

	void send_read_command()
	{
		// read operation always try read DATA_MAXSZ block (hdr->size ignored)
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: kCmdReadFile: %u off: %u", active_session, read_offset);
		FTPRequest req(FTPRequest::kCmdReadFile, active_session);
		req.header()->offset = read_offset;
		req.header()->size = 0 /* FTPRequest::DATA_MAXSZ */;
		req.send(m_uas, last_send_seqnr);
	}

	void send_write_command(const size_t bytes_to_copy)
	{
		// write chunk from write_buffer [write_it..bytes_to_copy]
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:m: kCmdWriteFile: %u off: %u sz: %z", active_session, write_offset, bytes_to_copy);
		FTPRequest req(FTPRequest::kCmdWriteFile, active_session);
		req.header()->offset = write_offset;
		req.header()->size = bytes_to_copy;
		std::copy(write_it, write_it + bytes_to_copy, req.data());
		req.send(m_uas, last_send_seqnr);
	}

	void send_remove_command(std::string &path) {
		send_any_path_command(FTPRequest::kCmdRemoveFile, "kCmdRemoveFile: ", path, 0);
	}

	bool send_rename_command(std::string &old_path, std::string &new_path)
	{
		std::ostringstream os;
		os << old_path;
		os << '\0';
		os << new_path;

		std::string paths = os.str();
		if (paths.size() >= FTPRequest::DATA_MAXSZ) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: rename file paths is too long: %zu", paths.size());
			r_errno = ENAMETOOLONG;
			return false;
		}

		send_any_path_command(FTPRequest::kCmdRename, "kCmdRename: ", paths, 0);
		
	}

	void send_truncate_command(std::string &path, size_t length) {
		send_any_path_command(FTPRequest::kCmdTruncateFile, "kCmdTruncateFile: ", path, length);
	}

	void send_create_dir_command(std::string &path) {
		send_any_path_command(FTPRequest::kCmdCreateDirectory, "kCmdCreateDirectory: ", path, 0);
	}

	void send_remove_dir_command(std::string &path) {
		send_any_path_command(FTPRequest::kCmdRemoveDirectory, "kCmdRemoveDirectory: ", path, 0);
	}

	void send_calc_file_crc32_command(std::string &path) {
		send_any_path_command(FTPRequest::kCmdCalcFileCRC32, "kCmdCalcFileCRC32: ", path, 0);
	}

	/* -*- helpers -*- */

	void add_dirent(const char *ptr, size_t slen)
	{
		mavros_msgs::msg::FileEntry ent;
		ent.size = 0;

		if (ptr[0] == FTPRequest::DIRENT_DIR) {
			ent.name.assign(ptr + 1, slen - 1);
			ent.type = mavros_msgs::msg::FileEntry::TYPE_DIRECTORY;

			RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:List Dir: %s", ent.name.c_str());
		}
		else {
			// ptr[0] == FTPRequest::DIRENT_FILE
			std::string name_size(ptr + 1, slen - 1);

			auto sep_it = std::find(name_size.begin(), name_size.end(), '\t');
			ent.name.assign(name_size.begin(), sep_it);
			ent.type = mavros_msgs::msg::FileEntry::TYPE_FILE;

			if (sep_it != name_size.end()) {
				name_size.erase(name_size.begin(), sep_it + 1);
				if (name_size.size() != 0)
					ent.size = std::stoi(name_size);
			}

			RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:List File: %s SZ: %llu", ent.name.c_str(), ent.size);
		}

		list_entries.push_back(ent);
	}

	void list_directory_end() {
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:List done");
		go_idle(false);
	}

	void list_directory(std::string &path)
	{
		list_offset = 0;
		list_path = path;
		list_entries.clear();
		op_state = OP::LIST;

		send_list_command();
	}

	bool open_file(std::string &path, int mode)
	{
		open_path = path;
		open_size = 0;
		op_state = OP::OPEN;

		if (mode == mavros_msgs::srv::FileOpen_Request::MODE_READ)
			send_open_ro_command();
		else if (mode == mavros_msgs::srv::FileOpen_Request::MODE_WRITE)
			send_open_wo_command();
		else if (mode == mavros_msgs::srv::FileOpen_Request::MODE_CREATE)
			send_create_command();
		else {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Unsupported open mode: %d", mode);
			op_state = OP::IDLE;
			r_errno = EINVAL;
			return false;
		}

		
	}

	bool close_file(std::string &path)
	{
		auto it = session_file_map.find(path);
		if (it == session_file_map.end()) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Close %s: not opened", path.c_str());
			r_errno = EBADF;
			return false;
		}

		op_state = OP::ACK;
		send_terminate_command(it->second);
		session_file_map.erase(it);
		
	}

	void read_file_end() {
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:Read done");
		go_idle(false);
	}

	bool read_file(std::string &path, size_t off, size_t len)
	{
		auto it = session_file_map.find(path);
		if (it == session_file_map.end()) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Read %s: not opened", path.c_str());
			r_errno = EBADF;
			return false;
		}

		op_state = OP::READ;
		active_session = it->second;
		read_size = len;
		read_offset = off;
		read_buffer.clear();
		if (read_buffer.capacity() < len ||
				read_buffer.capacity() > len + MAX_RESERVE_DIFF) {
			// reserve memory
			read_buffer.reserve(len);
		}

		send_read_command();
		
	}

	void write_file_end() {
		RCUTILS_LOG_DEBUG_NAMED("ftp", "FTP:Write done");
		go_idle(false);
	}

	bool write_file(std::string &path, size_t off, V_FileData &data)
	{
		auto it = session_file_map.find(path);
		if (it == session_file_map.end()) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP:Write %s: not opened", path.c_str());
			r_errno = EBADF;
			return false;
		}

		op_state = OP::WRITE;
		active_session = it->second;
		write_offset = off;
		write_buffer = std::move(data);
		write_it = write_buffer.begin();

		send_write_command(write_bytes_to_copy());
		
	}

	void remove_file(std::string &path) {
		op_state = OP::ACK;
		send_remove_command(path);
	}

	bool rename_(std::string &old_path, std::string &new_path) {
		op_state = OP::ACK;
		return send_rename_command(old_path, new_path);
	}

	void truncate_file(std::string &path, size_t length) {
		op_state = OP::ACK;
		send_truncate_command(path, length);
	}

	void create_directory(std::string &path) {
		op_state = OP::ACK;
		send_create_dir_command(path);
	}

	void remove_directory(std::string &path) {
		op_state = OP::ACK;
		send_remove_dir_command(path);
	}

	void checksum_crc32_file(std::string &path) {
		op_state = OP::CHECKSUM;
		checksum_crc32 = 0;
		send_calc_file_crc32_command(path);
	}

	static constexpr int compute_rw_timeout(size_t len) {
		return CHUNK_TIMEOUT_MS * (len / FTPRequest::DATA_MAXSZ + 1);
	}

	size_t write_bytes_to_copy() {
		return std::min<size_t>(std::distance(write_it, write_buffer.end()),
				FTPRequest::DATA_MAXSZ);
	}

	bool wait_completion(const int msecs)
	{
		std::unique_lock<std::mutex> lock(cond_mutex);

		bool is_timedout = cond.wait_for(lock, std::chrono::milliseconds(msecs))
				== std::cv_status::timeout;

		if (is_timedout) {
			// If timeout occurs don't forget to reset state
			op_state = OP::IDLE;
			r_errno = ETIMEDOUT;
			return false;
		}
		else
			// if go_idle() occurs before timeout
			return !is_error;
	}

	/* -*- service callbacks -*- */

	/**
	 * Service handler common header code.
	 */
#define SERVICE_IDLE_CHECK()				\
	if (op_state != OP::IDLE) {			\
		RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: Busy");	\
		return;				\
	}

	void list_cb(const mavros_msgs::srv::FileList::Request::SharedPtr req,
			mavros_msgs::srv::FileList::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		list_directory(req->dir_path);
		res->success = wait_completion(LIST_TIMEOUT_MS);
		res->r_errno = r_errno;
		if (res->success) {
			res->list = std::move(list_entries);
			list_entries.clear();	// not shure that it's needed
		}
	}

	void open_cb(const mavros_msgs::srv::FileOpen::Request::SharedPtr req,
			mavros_msgs::srv::FileOpen::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		// only one session per file
		auto it = session_file_map.find(req->file_path);
		if (it != session_file_map.end()) {
			RCUTILS_LOG_ERROR_NAMED("ftp", "FTP: File %s: already opened",
					req->file_path.c_str());
			res->success = false;
			return;
		}

		res->success = open_file(req->file_path, req->mode);
		if (res->success) {
			res->success = wait_completion(OPEN_TIMEOUT_MS);
			res->size = open_size;
		}
		res->r_errno = r_errno;
	}

	void close_cb(const mavros_msgs::srv::FileClose::Request::SharedPtr req,
			mavros_msgs::srv::FileClose::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		res->success = close_file(req->file_path);
		if (res->success) {
			res->success = wait_completion(OPEN_TIMEOUT_MS);
		}
		res->r_errno = r_errno;

		
	}

	void read_cb(const mavros_msgs::srv::FileRead::Request::SharedPtr req,
			mavros_msgs::srv::FileRead::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		res->success = read_file(req->file_path, req->offset, req->size);
		if (res->success)
			res->success = wait_completion(compute_rw_timeout(req->size));
		if (res->success) {
			res->data = std::move(read_buffer);
			read_buffer.clear();	// same as for list_entries
		}
		res->r_errno = r_errno;

		
	}

	void write_cb(const mavros_msgs::srv::FileWrite::Request::SharedPtr req,
			mavros_msgs::srv::FileWrite::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		const size_t data_size = req->data.size();
		res->success = write_file(req->file_path, req->offset, req->data);
		if (res->success) {
			res->success = wait_completion(compute_rw_timeout(data_size));
		}
		write_buffer.clear();
		res->r_errno = r_errno;

		
	}

	void remove_cb(const mavros_msgs::srv::FileRemove::Request::SharedPtr req,
			mavros_msgs::srv::FileRemove::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		remove_file(req->file_path);
		res->success = wait_completion(OPEN_TIMEOUT_MS);
		res->r_errno = r_errno;

		
	}

	void rename_cb(const mavros_msgs::srv::FileRename::Request::SharedPtr req,
			mavros_msgs::srv::FileRename::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		res->success = rename_(req->old_path, req->new_path);
		if (res->success) {
			res->success = wait_completion(OPEN_TIMEOUT_MS);
		}
		res->r_errno = r_errno;

		
	}


	void truncate_cb(const mavros_msgs::srv::FileTruncate::Request::SharedPtr req,
			mavros_msgs::srv::FileTruncate::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		// Note: emulated truncate() can take a while
		truncate_file(req->file_path, req->length);
		res->success = wait_completion(LIST_TIMEOUT_MS * 5);
		res->r_errno = r_errno;

		
	}

	void mkdir_cb(const mavros_msgs::srv::FileMakeDir::Request::SharedPtr req,
			mavros_msgs::srv::FileMakeDir::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		create_directory(req->dir_path);
		res->success = wait_completion(OPEN_TIMEOUT_MS);
		res->r_errno = r_errno;

		
	}

	void rmdir_cb(const mavros_msgs::srv::FileRemoveDir::Request::SharedPtr req,
			mavros_msgs::srv::FileRemoveDir::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		remove_directory(req->dir_path);
		res->success = wait_completion(OPEN_TIMEOUT_MS);
		res->r_errno = r_errno;

		
	}

	void checksum_cb(const mavros_msgs::srv::FileChecksum::Request::SharedPtr req,
			mavros_msgs::srv::FileChecksum::Response::SharedPtr res)
	{
		SERVICE_IDLE_CHECK();

		checksum_crc32_file(req->file_path);
		res->success = wait_completion(LIST_TIMEOUT_MS);
		res->crc32 = checksum_crc32;
		res->r_errno = r_errno;

		
	}

#undef SERVICE_IDLE_CHECK

	/**
	 * @brief Reset communication on both sides.
	 * @note This call break other calls, so use carefully.
	 */
	void reset_cb(const std_srvs::srv::Empty::Request::SharedPtr req,
			std_srvs::srv::Empty::Response::SharedPtr res)
	{
		send_reset();
	}
};
}	// namespace std_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(mavros::std_plugins::FTPPlugin, mavros::plugin::PluginBase)
