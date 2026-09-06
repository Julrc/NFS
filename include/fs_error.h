#pragma once 

#include <stdexcept>
#include <string>

enum class FSErr
{
	NotFound,
	AlreadyExists,
	InvalidPath,
	NoSpace,
	NotADirectory,
	ReadError,
	WriteError,
	SyncError,
	MustNotBeDirectory,
};

inline const char* fs_err_str(FSErr e)
{
	switch(e)
	{
		case FSErr::NotFound:		return "no such file or directory";
		case FSErr::AlreadyExists: return "file already exists";
		case FSErr::InvalidPath: return "invalid path";
		case FSErr::NoSpace: return "no space left on device";
		case FSErr::NotADirectory: return "not a directory";
		case FSErr::ReadError: return "a read error has occurred";
		case FSErr::WriteError: return "a write error has occurred";
		case FSErr::SyncError: return "a sync error has occurred, unable to write to disk";
		case FSErr::MustNotBeDirectory: return "do not specify a directory";
	}
	return "unknown error";
}

struct FSError : std::runtime_error
{
	FSErr code;
	FSError(FSErr c, const std::string& msg) : std::runtime_error(msg.empty() ? fs_err_str(c) : msg + ": " + fs_err_str(c)), code(c) {}
};
