// Copyrighted (C) 2015-2016 Antinet.org team, see file LICENCE-by-Antinet.txt
#if defined(_WIN32) || defined(__CYGWIN__)
	#define UNICODE
	#define _UNICODE
#endif

#include "c_tun_device.hpp"

#include "c_tnetdbg.hpp"
#ifdef __linux__
#include <cassert>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "c_tnetdbg.hpp"
#include "../depends/cjdns-code/NetPlatform.h"
#include "cpputils.hpp"
c_tun_device_linux::c_tun_device_linux()
:
	m_tun_fd(open("/dev/net/tun", O_RDWR))
{
	assert(! (m_tun_fd<0) ); // TODO throw?
}

void c_tun_device_linux::set_ipv6_address
	(const std::array<uint8_t, 16> &binary_address, int prefixLen) {
	as_zerofill< ifreq > ifr; // the if request
	ifr.ifr_flags = IFF_TUN; // || IFF_MULTI_QUEUE; TODO
	strncpy(ifr.ifr_name, "galaxy%d", IFNAMSIZ);
	auto errcode_ioctl =  ioctl(m_tun_fd, TUNSETIFF, static_cast<void *>(&ifr));
	if (errcode_ioctl < 0) _throw_error( std::runtime_error("ioctl error") );
	assert(binary_address[0] == 0xFD);
	assert(binary_address[1] == 0x42);
	NetPlatform_addAddress(ifr.ifr_name, binary_address.data(), prefixLen, Sockaddr_AF_INET6);
}

void c_tun_device_linux::set_mtu(uint32_t mtu) {
	_UNUSED(mtu);
	_NOTREADY();
}

bool c_tun_device_linux::incomming_message_form_tun() {
	fd_set fd_set_data;
	FD_ZERO(&fd_set_data);
	FD_SET(m_tun_fd, &fd_set_data);
	timeval timeout { 0 , 500 }; // http://pubs.opengroup.org/onlinepubs/007908775/xsh/systime.h.html
	auto select_result = select( m_tun_fd+1, &fd_set_data, nullptr, nullptr, & timeout); // <--- blocks
	_assert(select_result >= 0);
	if (FD_ISSET(m_tun_fd, &fd_set_data)) return true;
	else return false;
}

size_t c_tun_device_linux::read_from_tun(void *buf, size_t count) { // TODO throw if error
	ssize_t ret = read(m_tun_fd, buf, count); // <-- read data from TUN
	if (ret == -1) _throw_error( std::runtime_error("Read from tun error") );
	assert (ret >= 0);
	return static_cast<size_t>(ret);
}

size_t c_tun_device_linux::write_to_tun(const void *buf, size_t count) { // TODO throw if error
	auto ret = write(m_tun_fd, buf, count);
	if (ret == -1) _throw_error( std::runtime_error("Write to tun error") );
	assert (ret >= 0);
	return static_cast<size_t>(ret);
}

#endif //__linux__

#if defined(_WIN32) || defined(__CYGWIN__)

#include "c_tnetdbg.hpp"
#include <boost/bind.hpp>
#include <cassert>
#include <ifdef.h>
#include <io.h>
//#include <ntdef.h>
//#include <ntstatus.h>
#ifndef NTSTATUS 
#define NTSTATUS LONG
#endif 
#include <netioapi.h>
#include <ntddscsi.h>
#include <winioctl.h>

#define TAP_CONTROL_CODE(request,method) \
  CTL_CODE (FILE_DEVICE_UNKNOWN, request, method, FILE_ANY_ACCESS)
#define TAP_IOCTL_GET_MAC				TAP_CONTROL_CODE (1, METHOD_BUFFERED)
#define TAP_IOCTL_GET_VERSION			TAP_CONTROL_CODE (2, METHOD_BUFFERED)
#define TAP_IOCTL_SET_MEDIA_STATUS		TAP_CONTROL_CODE (6, METHOD_BUFFERED)

#if defined (__MINGW32__)
	#undef _assert
#endif

c_tun_device_windows::c_tun_device_windows()
	:
	// LOG_ON_INIT( _note("Creating TUN device (windows)") ),
	m_guid(get_device_guid()),
	m_readed_bytes(0),
	m_handle(get_device_handle()),
	m_stream_handle_ptr(std::make_unique<boost::asio::windows::stream_handle>(m_ioservice, m_handle)),
	m_mac_address(get_mac(m_handle))
{
	m_buffer.fill(0);
	assert(m_stream_handle_ptr->is_open());
	//m_stream_handle_ptr->async_read_some(boost::asio::buffer(m_buffer), std::bind(&c_tun_device_windows::handle_read, this));
	m_stream_handle_ptr->async_read_some(boost::asio::buffer(m_buffer),
			boost::bind(&c_tun_device_windows::handle_read, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
}

void c_tun_device_windows::set_ipv6_address
(const std::array<uint8_t, 16> &binary_address, int prefixLen) {
	auto human_name = get_human_name(m_guid);
	auto luid = get_luid(human_name);
	// remove old address
	MIB_UNICASTIPADDRESS_TABLE *table = nullptr;
	GetUnicastIpAddressTable(AF_INET6, &table);
	for (int i = 0; i < static_cast<int>(table->NumEntries); ++i) {
		if (table->Table[i].InterfaceLuid.Value == luid.Value)
			if (DeleteUnicastIpAddressEntry(&table->Table[i]) != NO_ERROR)
				throw std::runtime_error("DeleteUnicastIpAddressEntry error");
	}
	FreeMibTable(table);

	// set new address
	MIB_UNICASTIPADDRESS_ROW iprow;
	std::memset(&iprow, 0, sizeof(iprow));
	iprow.PrefixOrigin = IpPrefixOriginUnchanged;
	iprow.SuffixOrigin = IpSuffixOriginUnchanged;
	iprow.ValidLifetime = 0xFFFFFFFF;
	iprow.PreferredLifetime = 0xFFFFFFFF;
	iprow.OnLinkPrefixLength = 0xFF;

	iprow.InterfaceLuid = luid;
	iprow.Address.si_family = AF_INET6;
	std::memcpy(&iprow.Address.Ipv6.sin6_addr, binary_address.data(), binary_address.size());
	iprow.OnLinkPrefixLength = prefixLen;

	auto status = CreateUnicastIpAddressEntry(&iprow);
	// TODO check for error with status
	_UNUSED(status);
}

bool c_tun_device_windows::incomming_message_form_tun() {
	m_ioservice.run_one(); // <--- will call ASIO handler if there is any new data
	if (m_readed_bytes > 0) return true;
	return false;
}

size_t c_tun_device_windows::read_from_tun(void *buf, size_t count) {
	const size_t eth_offset = 10;
	m_readed_bytes -= eth_offset;
	assert(m_readed_bytes > 0);
	std::copy_n(&m_buffer[0] + eth_offset, m_readed_bytes, reinterpret_cast<uint8_t*>(buf)); // TODO!!! change base api and remove copy!!!
	size_t ret = m_readed_bytes;
	m_readed_bytes = 0;
	return ret;
}

size_t c_tun_device_windows::write_to_tun(const void *buf, size_t count) {
	//std::cout << "****************write to tun" << std::endl;
	const size_t eth_header_size = 14;
	const size_t eth_offset = 4;
	std::vector<uint8_t> eth_frame(eth_header_size + count - eth_offset, 0);
	std::copy(m_mac_address.begin(), m_mac_address.end(), eth_frame.begin()); // destination mac address
	auto it = eth_frame.begin() + 6;
	// source mac address
	*it = 0xFC; ++it;
	for (int i = 0; i < 5; ++i) {
		*it = 0x00; ++it;
	}
	// eth type: ipv6
	*it = 0x86; ++it;
	*it = 0xDD; ++it;
	std::copy(reinterpret_cast<const uint8_t *>(buf) + eth_offset, reinterpret_cast<const uint8_t *>(buf) + count, it);
	boost::system::error_code ec;
	//size_t write_bytes = m_stream_handle_ptr->write_some(boost::asio::buffer(buf, count), ec); // prepares: blocks (but TUN is fast)
	size_t write_bytes = m_stream_handle_ptr->write_some(boost::asio::buffer(eth_frame), ec); // prepares: blocks (but TUN is fast)
	if (ec) throw std::runtime_error("boost error " + ec.message());
	return write_bytes;
}

// base on https://msdn.microsoft.com/en-us/library/windows/desktop/ms724256(v=vs.85).aspx
#define MAX_KEY_LENGTH 255
#define MAX_VALUE_NAME 16383
std::vector<std::wstring> c_tun_device_windows::get_subkeys(HKEY hKey) {
	TCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
	DWORD    cbName;                   // size of name string
	TCHAR    achClass[MAX_PATH] = TEXT("");  // buffer for class name
	DWORD    cchClassName = MAX_PATH;  // size of class string
	DWORD    cSubKeys = 0;               // number of subkeys
	DWORD    cbMaxSubKey;              // longest subkey size
	DWORD    cchMaxClass;              // longest class string
	DWORD    cValues;              // number of values for key
	DWORD    cchMaxValue;          // longest value name
	DWORD    cbMaxValueData;       // longest value data
	DWORD    cbSecurityDescriptor; // size of security descriptor
	FILETIME ftLastWriteTime;      // last write time
	DWORD i, retCode;
	std::vector<std::wstring> ret;
	TCHAR  achValue[MAX_VALUE_NAME];
	DWORD cchValue = MAX_VALUE_NAME;

	// Get the class name and the value count.
	retCode = RegQueryInfoKey(
		hKey,                    // key handle
		achClass,                // buffer for class name
		&cchClassName,           // size of class string
		NULL,                    // reserved
		&cSubKeys,               // number of subkeys
		&cbMaxSubKey,            // longest subkey size
		&cchMaxClass,            // longest class string
		&cValues,                // number of values for this key
		&cchMaxValue,            // longest value name
		&cbMaxValueData,         // longest value data
		&cbSecurityDescriptor,   // security descriptor
		&ftLastWriteTime);       // last write time
								 // Enumerate the subkeys, until RegEnumKeyEx fails.
	if (cSubKeys) {
		std::cout << "Number of subkeys: " << cSubKeys << std::endl;

		for (i = 0; i < cSubKeys; i++) {
			cbName = MAX_KEY_LENGTH;
			retCode = RegEnumKeyEx(hKey, i,
				achKey,
				&cbName,
				NULL,
				NULL,
				NULL,
				&ftLastWriteTime);
			if (retCode == ERROR_SUCCESS) {
				//std::wcout << achKey << std::endl;
				//std::cout << "get value" << std::endl;
				ret.emplace_back(std::wstring(achKey));
			}
		}
	}
	return ret;
}

std::wstring c_tun_device_windows::get_device_guid() {
	const std::wstring adapterKey = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
	LONG status = 1;
	HKEY key = nullptr;
	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, adapterKey.c_str(), 0, KEY_READ, &key);
	if (status != ERROR_SUCCESS) throw std::runtime_error("RegOpenKeyEx error, error code " + std::to_string(GetLastError()));
	auto subkeys_vector = get_subkeys(key);
	RegCloseKey(key);
	for (auto & subkey : subkeys_vector) { // foreach sub key
		if (subkey == L"Properties") continue;
		std::wstring subkey_reg_path = adapterKey + L"\\" + subkey;
		// std::wcout << subkey_reg_path << std::endl;
		status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey_reg_path.c_str(), 0, KEY_QUERY_VALUE, &key);
		if (status != ERROR_SUCCESS) throw std::runtime_error("RegOpenKeyEx error, error code " + std::to_string(GetLastError()));
		// get ComponentId field
		DWORD size = 256;
		std::wstring componentId(size, '\0');
		status = RegQueryValueExW(key, L"ComponentId", nullptr, nullptr, reinterpret_cast<LPBYTE>(&componentId[0]), &size);
		if (status != ERROR_SUCCESS) {
			RegCloseKey(key);
			continue;
		}
		if (componentId.substr(0, 8) == L"root\\tap" || componentId.substr(0, 3) == L"tap") { // found TAP
			std::wcout << subkey_reg_path << std::endl;
			size = 256;
			std::wstring netCfgInstanceId(size, '\0');
			status = RegQueryValueExW(key, L"NetCfgInstanceId", nullptr, nullptr, reinterpret_cast<LPBYTE>(&netCfgInstanceId[0]), &size);
			if (status != ERROR_SUCCESS) throw std::runtime_error("RegQueryValueEx error, error code " + std::to_string(GetLastError()));
			netCfgInstanceId.erase(size / sizeof(wchar_t) - 1); // remove '\0'
			std::wcout << netCfgInstanceId << std::endl;
			RegCloseKey(key);
			HANDLE handle = open_tun_device(netCfgInstanceId);
			if (handle == INVALID_HANDLE_VALUE) continue;
			else CloseHandle(handle);
			return netCfgInstanceId;
		}
		RegCloseKey(key);
	}
	throw std::runtime_error("Device not found");
}

std::wstring c_tun_device_windows::get_human_name(const std::wstring &guid) {
	assert(!guid.empty());
	std::wstring connectionKey = L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
	connectionKey += guid;
	connectionKey += L"\\Connection";
	std::wcout << "connectionKey " << connectionKey << L"*******" << std::endl;
	LONG status = 1;
	HKEY key = nullptr;
	DWORD size = 256;
	std::wstring name(size, '\0');
	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, connectionKey.c_str(), 0, KEY_QUERY_VALUE, &key);
	status = RegQueryValueExW(key, L"Name", nullptr, nullptr, reinterpret_cast<LPBYTE>(&name[0]), &size);
	name.erase(size / sizeof(wchar_t) - 1); // remove '\0'
	RegCloseKey(key);
	return name;
}

NET_LUID c_tun_device_windows::get_luid(const std::wstring &human_name) {
	NET_LUID ret;
	auto status = ConvertInterfaceAliasToLuid(human_name.c_str(), &ret); // TODO throw
	if (status != ERROR_SUCCESS) throw std::runtime_error("ConvertInterfaceAliasToLuid error, error code " + std::to_string(GetLastError()));
	return ret;
}


HANDLE c_tun_device_windows::get_device_handle() {
	HANDLE handle = open_tun_device(m_guid);
	if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("invalid handle");
	// get version
	ULONG version_len;
	struct {
		unsigned long major;
		unsigned long minor;
		unsigned long debug;
	} version;
	BOOL bret = DeviceIoControl(handle, TAP_IOCTL_GET_VERSION, &version, sizeof(version), &version, sizeof(version), &version_len, nullptr);
	if (bret == false) {
		CloseHandle(handle);
		throw std::runtime_error("DeviceIoControl error");
	}
	// set status
	int status = 1;
	unsigned long len = 0;
	bret = DeviceIoControl(handle, TAP_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status), &status, sizeof(status), &len, nullptr);
	if (bret == false) {
		CloseHandle(handle);
		throw std::runtime_error("DeviceIoControl error");
	}
	return handle;
}

HANDLE c_tun_device_windows::open_tun_device(const std::wstring &guid) {
	std::wstring tun_filename;
	tun_filename += L"\\\\.\\Global\\";
	tun_filename += guid;
	tun_filename += L".tap";
	BOOL bret;
	HANDLE handle = CreateFileW(tun_filename.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		0,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED,
		0);
	return handle;
}

std::array<uint8_t, 6> c_tun_device_windows::get_mac(HANDLE handle) {
	std::array<uint8_t, 6> mac_address;
	DWORD mac_size = 0;
	BOOL bret = DeviceIoControl(handle, TAP_IOCTL_GET_MAC, &mac_address.front(), mac_address.size(), &mac_address.front(), mac_address.size(), &mac_size, nullptr);
	assert(mac_size == mac_address.size());
	for (const auto i : mac_address)
		std::cout << std::hex << static_cast<int>(i) << " ";
	std::cout << std::dec << std::endl;
	return mac_address;
}

void c_tun_device_windows::handle_read(const boost::system::error_code& error, std::size_t length) {
	//std::cout << "tun handle read" << std::endl;
	//std::cout << "readed " << length << " bytes from tun" << std::endl;

	try {
		if (error || (length < 1)) throw std::runtime_error(error.message());
		if (length < 54) throw std::runtime_error("tun data length < 54"); // 54 == sum of header sizes

		m_readed_bytes = length;
		if (c_ndp::is_packet_neighbor_solicitation(m_buffer)) {
			std::array<uint8_t, 94> neighbor_advertisement_packet = c_ndp::generate_neighbor_advertisement(m_buffer);
			boost::system::error_code ec;
			m_stream_handle_ptr->write_some(boost::asio::buffer(neighbor_advertisement_packet), ec); // prepares: blocks (but TUN is fast)
		}
	}
	catch (const std::runtime_error &e) {
		m_readed_bytes = 0;
		_erro("Problem with the TUN/TAP parser" << std::endl << e.what());
	}

	// continue reading
	m_stream_handle_ptr->async_read_some(boost::asio::buffer(m_buffer),
			boost::bind(&c_tun_device_windows::handle_read, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
}

#endif



c_tun_device_empty::c_tun_device_empty() { }

void c_tun_device_empty::set_ipv6_address(const std::array<uint8_t, 16> &binary_address, int prefixLen) {
	_UNUSED(binary_address);
	_UNUSED(prefixLen);
}

void c_tun_device_empty::set_mtu(uint32_t mtu) {
	_UNUSED(mtu);
}

bool c_tun_device_empty::incomming_message_form_tun() {
	return false;
}

size_t c_tun_device_empty::read_from_tun(void *buf, size_t count) {
	_UNUSED(buf);
	_UNUSED(count);
	return 0;
}

size_t c_tun_device_empty::write_to_tun(const void *buf, size_t count) {
	_UNUSED(buf);
	_UNUSED(count);
	return 0;
}


