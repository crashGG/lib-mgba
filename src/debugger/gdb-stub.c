#include "gdb-stub.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum GDBError {
	GDB_NO_ERROR = 0x00,
	GDB_BAD_ARGUMENTS = 0x06,
	GDB_UNSUPPORTED_COMMAND = 0x07
};

enum {
	MACH_O_ARM = 12,
	MACH_O_ARM_V4T = 5
};

static void _sendMessage(struct GDBStub* stub);

static void _gdbStubDeinit(struct ARMDebugger* debugger) {
	struct GDBStub* stub = (struct GDBStub*) debugger;
	if (stub->socket >= 0) {
		GDBStubShutdown(stub);
	}
}

static void _gdbStubEntered(struct ARMDebugger* debugger, enum DebuggerEntryReason reason) {
	struct GDBStub* stub = (struct GDBStub*) debugger;
	switch (reason) {
	case DEBUGGER_ENTER_MANUAL:
		snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "S%02x", SIGINT);
		break;
	case DEBUGGER_ENTER_BREAKPOINT:
	case DEBUGGER_ENTER_WATCHPOINT: // TODO: Make watchpoints raise with address
		snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "S%02x", SIGTRAP);
		break;
	case DEBUGGER_ENTER_ILLEGAL_OP:
		snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "S%02x", SIGILL);
		break;
	case DEBUGGER_ENTER_ATTACHED:
		return;
	}
	_sendMessage(stub);
}

static void _gdbStubPoll(struct ARMDebugger* debugger) {
	struct GDBStub* stub = (struct GDBStub*) debugger;
	int flags;
	while (stub->d.state == DEBUGGER_PAUSED) {
		if (stub->connection >= 0) {
			flags = fcntl(stub->connection, F_GETFL);
			if (flags == -1) {
				GDBStubHangup(stub);
				return;
			}
			fcntl(stub->connection, F_SETFL, flags & ~O_NONBLOCK);
		}
		GDBStubUpdate(stub);
	}
}

static void _ack(struct GDBStub* stub) {
	char ack = '+';
	send(stub->connection, &ack, 1, 0);
}

static void _nak(struct GDBStub* stub) {
	char nak = '-';
	printf("Packet error\n");
	send(stub->connection, &nak, 1, 0);
}

static uint32_t _hex2int(const char* hex, int maxDigits) {
	uint32_t value = 0;
	uint8_t letter;

	while (maxDigits--) {
		letter = *hex - '0';
		if (letter > 9) {
			letter = *hex - 'a';
			if  (letter > 5) {
				break;
			}
			value *= 0x10;
			value += letter + 10;
		} else {
			value *= 0x10;
			value += letter;
		}
		++hex;
	}
	return value;
}

static void _int2hex8(uint8_t value, char* out) {
	static const char language[] = "0123456789abcdef";
	out[0] = language[value >> 4];
	out[1] = language[value & 0xF];
}

static void _int2hex32(uint32_t value, char* out) {
	static const char language[] = "0123456789abcdef";
	out[6] = language[value >> 28];
	out[7] = language[(value >> 24) & 0xF];
	out[4] = language[(value >> 20) & 0xF];
	out[5] = language[(value >> 16) & 0xF];
	out[2] = language[(value >> 12) & 0xF];
	out[3] = language[(value >> 8) & 0xF];
	out[0] = language[(value >> 4) & 0xF];
	out[1] = language[value & 0xF];
}

static uint32_t _readHex(const char* in, unsigned* out) {
	unsigned i;
	for (i = 0; i < 8; ++i) {
		if (in[i] == ',') {
			break;
		}
	}
	*out += i;
	return _hex2int(in, i);
}

static void _sendMessage(struct GDBStub* stub) {
	if (stub->lineAck != GDB_ACK_OFF) {
		stub->lineAck = GDB_ACK_PENDING;
	}
	uint8_t checksum = 0;
	int i = 1;
	char buffer = stub->outgoing[0];
	char swap;
	stub->outgoing[0] = '$';
	if (buffer) {
		for (; i < GDB_STUB_MAX_LINE - 5; ++i) {
			checksum += buffer;
			swap = stub->outgoing[i];
			stub->outgoing[i] = buffer;
			buffer = swap;
			if (!buffer) {
				++i;
				break;
			}
		}
	}
	stub->outgoing[i] = '#';
	_int2hex8(checksum, &stub->outgoing[i + 1]);
	stub->outgoing[i + 3] = 0;
	send(stub->connection, stub->outgoing, i + 3, 0);
}

static void _error(struct GDBStub* stub, enum GDBError error) {
	snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "E%02x", error);
	_sendMessage(stub);
}

static void _writeHostInfo(struct GDBStub* stub) {
	snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "cputype:%u;cpusubtype:%u:ostype:none;vendor:none;endian:little;ptrsize:4;", MACH_O_ARM, MACH_O_ARM_V4T);
	_sendMessage(stub);
}

static void _continue(struct GDBStub* stub, const char* message) {
	stub->d.state = DEBUGGER_RUNNING;
	if (stub->connection >= 0) {
		int flags = fcntl(stub->connection, F_GETFL);
		if (flags == -1) {
			GDBStubHangup(stub);
			return;
		}
		fcntl(stub->connection, F_SETFL, flags | O_NONBLOCK);
	}
	// TODO: parse message
	(void) (message);
}

static void _step(struct GDBStub* stub, const char* message) {
	ARMRun(stub->d.cpu);
	snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "S%02x", SIGINT);
	_sendMessage(stub);
	// TODO: parse message
	(void) (message);
}

static void _readMemory(struct GDBStub* stub, const char* message) {
	const char* readAddress = message;
	unsigned i = 0;
	uint32_t address = _readHex(readAddress, &i);
	readAddress += i + 1;
	uint32_t size = _readHex(readAddress, &i);
	if (size > 512) {
		_error(stub, GDB_BAD_ARGUMENTS);
		return;
	}
	struct ARMMemory* memory = stub->d.memoryShim.original;
	int writeAddress = 0;
	for (i = 0; i < size; ++i, writeAddress += 2) {
		uint8_t byte = memory->load8(memory, address + i, 0);
		_int2hex8(byte, &stub->outgoing[writeAddress]);
	}
	stub->outgoing[writeAddress] = 0;
	_sendMessage(stub);
}

static void _readGPRs(struct GDBStub* stub, const char* message) {
	(void) (message);
	int r;
	int i = 0;
	for (r = 0; r < 16; ++r) {
		_int2hex32(stub->d.cpu->gprs[r], &stub->outgoing[i]);
		i += 8;
	}
	stub->outgoing[i] = 0;
	_sendMessage(stub);
}

static void _readRegister(struct GDBStub* stub, const char* message) {
	const char* readAddress = message;
	unsigned i = 0;
	uint32_t reg = _readHex(readAddress, &i);
	uint32_t value;
	if (reg < 0x10) {
		value = stub->d.cpu->gprs[reg];
	} else if (reg == 0x19) {
		value = stub->d.cpu->cpsr.packed;
	} else {
		stub->outgoing[0] = '\0';
		_sendMessage(stub);
		return;
	}
	_int2hex32(value, stub->outgoing);
	stub->outgoing[8] = '\0';
	_sendMessage(stub);
}

static void _processQReadCommand(struct GDBStub* stub, const char* message) {
	stub->outgoing[0] = '\0';
	if (!strncmp("HostInfo#", message, 9)) {
		_writeHostInfo(stub);
		return;
	}
	if (!strncmp("Attached#", message, 9)) {
		strncpy(stub->outgoing, "1", GDB_STUB_MAX_LINE - 4);
	} else if (!strncmp("VAttachOrWaitSupported#", message, 23)) {
		strncpy(stub->outgoing, "OK", GDB_STUB_MAX_LINE - 4);
	} else if (!strncmp("C#", message, 2)) {
		strncpy(stub->outgoing, "QC1", GDB_STUB_MAX_LINE - 4);
	} else if (!strncmp("fThreadInfo#", message, 12)) {
		strncpy(stub->outgoing, "m1", GDB_STUB_MAX_LINE - 4);
	} else if (!strncmp("sThreadInfo#", message, 12)) {
		strncpy(stub->outgoing, "l", GDB_STUB_MAX_LINE - 4);
	}
	_sendMessage(stub);
}

static void _processQWriteCommand(struct GDBStub* stub, const char* message) {
	stub->outgoing[0] = '\0';
	if (!strncmp("StartNoAckMode#", message, 16)) {
		stub->lineAck = GDB_ACK_OFF;
		strncpy(stub->outgoing, "OK", GDB_STUB_MAX_LINE - 4);
	}
	_sendMessage(stub);
}

static void _processVWriteCommand(struct GDBStub* stub, const char* message) {
	(void) (message);
	stub->outgoing[0] = '\0';
	_sendMessage(stub);
}

static void _processVReadCommand(struct GDBStub* stub, const char* message) {
	stub->outgoing[0] = '\0';
	if (!strncmp("Attach", message, 6)) {
		strncpy(stub->outgoing, "1", GDB_STUB_MAX_LINE - 4);
		ARMDebuggerEnter(&stub->d, DEBUGGER_ENTER_MANUAL);
	}
	_sendMessage(stub);
}

static void _setBreakpoint(struct GDBStub* stub, const char* message) {
	switch (message[0]) {
	case '0': // Memory breakpoints are not currently supported
	case '1': {
		const char* readAddress = &message[2];
		unsigned i = 0;
		uint32_t address = _readHex(readAddress, &i);
		readAddress += i + 1;
		uint32_t kind = _readHex(readAddress, &i); // We don't use this in hardware watchpoints
		(void) (kind);
		ARMDebuggerSetBreakpoint(&stub->d, address);
		strncpy(stub->outgoing, "OK", GDB_STUB_MAX_LINE - 4);
		_sendMessage(stub);
		break;
	}
	case '2':
	case '3':
		// TODO: Watchpoints
	default:
		break;
	}
}

static void _clearBreakpoint(struct GDBStub* stub, const char* message) {
	switch (message[0]) {
	case '0': // Memory breakpoints are not currently supported
	case '1': {
		const char* readAddress = &message[2];
		unsigned i = 0;
		uint32_t address = _readHex(readAddress, &i);
		ARMDebuggerClearBreakpoint(&stub->d, address);
		strncpy(stub->outgoing, "OK", GDB_STUB_MAX_LINE - 4);
		_sendMessage(stub);
		break;
	}
	case '2':
	case '3':
		// TODO: Watchpoints
	default:
		break;
	}
}

size_t _parseGDBMessage(struct GDBStub* stub, const char* message) {
	uint8_t checksum = 0;
	int parsed = 1;
	switch (*message) {
	case '+':
		stub->lineAck = GDB_ACK_RECEIVED;
		return parsed;
	case '-':
		stub->lineAck = GDB_NAK_RECEIVED;
		return parsed;
	case '$':
		++message;
		break;
	case '\x03':
		ARMDebuggerEnter(&stub->d, DEBUGGER_ENTER_MANUAL);
		return parsed;
	default:
		_nak(stub);
		return parsed;
	}

	int i;
	char messageType = message[0];
	for (i = 0; message[i] && message[i] != '#'; ++i, ++parsed) {
		checksum += message[i];
	}
	if (!message[i]) {
		_nak(stub);
		return parsed;
	}
	++i;
	++parsed;
	if (!message[i]) {
		_nak(stub);
		return parsed;
	} else if (!message[i + 1]) {
		++parsed;
		_nak(stub);
		return parsed;
	}
	parsed += 2;
	int networkChecksum = _hex2int(&message[i], 2);
	if (networkChecksum != checksum) {
		printf("Checksum error: expected %02x, got %02x\n", checksum, networkChecksum);
		_nak(stub);
		return parsed;
	}

	_ack(stub);
	++message;
	switch (messageType) {
	case '?':
		snprintf(stub->outgoing, GDB_STUB_MAX_LINE - 4, "S%02x", SIGINT);
		_sendMessage(stub);
		break;
	case 'c':
		_continue(stub, message);
		break;
	case 'g':
		_readGPRs(stub, message);
		break;
	case 'H':
		// This is faked because we only have one thread
		strncpy(stub->outgoing, "OK", GDB_STUB_MAX_LINE - 4);
		_sendMessage(stub);
		break;
	case 'm':
		_readMemory(stub, message);
		break;
	case 'p':
		_readRegister(stub, message);
		break;
	case 'Q':
		_processQWriteCommand(stub, message);
		break;
	case 'q':
		_processQReadCommand(stub, message);
		break;
	case 's':
		_step(stub, message);
		break;
	case 'V':
		_processVWriteCommand(stub, message);
		break;
	case 'v':
		_processVReadCommand(stub, message);
		break;
	case 'Z':
		_setBreakpoint(stub, message);
		break;
	case 'z':
		_clearBreakpoint(stub, message);
		break;
	default:
		_error(stub, GDB_UNSUPPORTED_COMMAND);
		break;
	}
	return parsed;
}

void GDBStubCreate(struct GDBStub* stub) {
	stub->socket = -1;
	stub->connection = -1;
	stub->d.init = 0;
	stub->d.deinit = _gdbStubDeinit;
	stub->d.paused = _gdbStubPoll;
	stub->d.entered = _gdbStubEntered;
}

int GDBStubListen(struct GDBStub* stub, int port, uint32_t bindAddress) {
	if (stub->socket >= 0) {
		GDBStubShutdown(stub);
	}
	// TODO: support IPv6
	stub->socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (stub->socket < 0) {
		printf("Couldn't open socket\n");
		return 0;
	}

	struct sockaddr_in bindInfo = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr = {
			.s_addr = htonl(bindAddress)
		}
	};
	int err = bind(stub->socket, (const struct sockaddr*) &bindInfo, sizeof(struct sockaddr_in));
	if (err) {
		goto cleanup;
	}
	err = listen(stub->socket, 1);
	if (err) {
		goto cleanup;
	}
	int flags = fcntl(stub->socket, F_GETFL);
	if (flags == -1) {
		goto cleanup;
	}
	fcntl(stub->socket, F_SETFL, flags | O_NONBLOCK);

	return 1;

cleanup:
	close(stub->socket);
	stub->socket = -1;
	return 0;
}

void GDBStubHangup(struct GDBStub* stub) {
	if (stub->connection >= 0) {
		close(stub->connection);
		stub->connection = -1;
	}
	if (stub->d.state == DEBUGGER_PAUSED) {
		stub->d.state = DEBUGGER_RUNNING;
	}
}

void GDBStubShutdown(struct GDBStub* stub) {
	GDBStubHangup(stub);
	if (stub->socket >= 0) {
		close(stub->socket);
		stub->socket = -1;
	}
}

void GDBStubUpdate(struct GDBStub* stub) {
	if (stub->connection == -1) {
		stub->connection = accept(stub->socket, 0, 0);
		if (stub->connection >= 0) {
			int flags = fcntl(stub->connection, F_GETFL);
			if (flags == -1) {
				goto connectionLost;
			}
			fcntl(stub->connection, F_SETFL, flags | O_NONBLOCK);
			ARMDebuggerEnter(&stub->d, DEBUGGER_ENTER_ATTACHED);
		} else if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return;
		} else {
			goto connectionLost;
		}
	}
	while (1) {
		ssize_t messageLen = recv(stub->connection, stub->line, GDB_STUB_MAX_LINE - 1, 0);
		if (messageLen == 0) {
			goto connectionLost;
		}
		if (messageLen == -1) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return;
			}
			goto connectionLost;
		}
		stub->line[messageLen] = '\0';
		ssize_t position = 0;
		while (position < messageLen) {
			position += _parseGDBMessage(stub, &stub->line[position]);
		}
	}

connectionLost:
	// TODO: add logging support to the debugging interface
	printf("Connection lost\n");
	GDBStubHangup(stub);
}
