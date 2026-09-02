#include "fs.h"


#include <cstring>

//  TODO: 
//  implement special directory bit to differentiate dir vs file inode
//  ONLY ONE OPEN SYSCALL , ONCE When constructing FS, CLOSE WHEN DESTRUCTING FD member variable
//  OFFSET OVERFLOWS WITH u32
//  REFACTOR CREATE_DIR 
//

void FS::mkdir(std::string path)
{
	// parse, directories to traverse inode from root

	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	// traverse
	
	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "mkdir: invalid path\n"); return; }

	if (find_in_dir(target_parent.value(), name))
	{
		fprintf(stderr, "mkdir: directory already exists\n");
		return;
	}
	if (!create_dir(target_parent.value(), name))
	{
		fprintf(stderr, "mkdir: error creating directory\n");
		return;
	}
	sync();
	// ex. /root/etc
}

void FS::ls(std::string path)
{
	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	u32 target = 0; // default for root
	
	if (!path_parts.empty())
	{
		std::string name;
		auto target_parent = resolve_parent(path_parts, name);
		if (!target_parent) { fprintf(stderr, "ls: invalid path\n"); return; }

		auto target_inode = find_in_dir(target_parent.value(), name);
		if (!target_inode) { fprintf(stderr, "ls: no such directory\n"); return; }
		target = *target_inode;
	}
	// out put target directories
	
	auto meta = read_inode_meta(target);
	if (!meta) { fprintf(stderr, "ls: unable to read directory metadata\n"); return; }
	if (!is_dir(meta.value())) { fprintf(stderr, "invalid, not a directory\n"); }
	auto data = read_inode_data(meta);
	if (!data) { fprintf(stderr, "ls: unable to read directory data\n"); return;}

	std::vector<entry_t> directory_entries = data_parse_dirs(data.value());
	for (auto &entry : directory_entries)
	{
		printf("%u, %s\n", entry.inode_num, entry.name);
	}
}

// from current directory m_curr_dir, traverse to path
// later on save curr dir inode num to remove need to traverse from root
void FS::cd(std::string path)
{
	//traverse
	if (path.empty()) { m_curr_dir = "/"; }
	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	if (path_parts.empty())
	{
		m_curr_dir = "/";
		return;
	}

	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "mkdir: invalid path\n"); return; }
	// set curr dir or print error0
	auto new_dir = find_in_dir(target_parent.value(), name);
	if (new_dir)
	{ 
		auto new_dir_meta = read_inode_meta(new_dir.value());
		if (new_dir_meta)
		{
			if (is_dir(new_dir_meta.value()))
			{
				m_curr_dir = full_path;
			}
			else
			{
				fprintf(stderr, "invalid, not a directory\n");
				return;
			}
		}
	}
	else{ fprintf(stderr, "cd: path not found\n"); return; }

}

void FS::rmdir(std::string path)
{
	if (path.empty()) { fprintf(stderr, "missing operand\n"); return; }

	std::string full_path = make_path(path);

	if (full_path == "/") { fprintf(stderr, "cannot delete root\n"); return; }
	if (full_path == m_curr_dir) { fprintf(stderr, "cannot remove current directory\n"); return; }

	// refuse if deleting a descendant of current directory
	if (m_curr_dir.compare(0, full_path.size(), full_path) == 0){ fprintf(stderr, "cannot delete descendent of current directory\n"); return; }

	auto path_parts = split_path(full_path);
	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "rmdir: invalid path\n"); }

	if (rm_inode(target_parent.value(), name)) { sync(); };
}

void FS::touch(std::string path)
{
	if (path.empty()) { fprintf(stderr, "touch: missing operand\n"); return; }
	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "file: invalid path\n"); return; }

	if (find_in_dir(target_parent.value(), name))
	{
		fprintf(stderr, "touch: file already exists\n");
		return;
	}
	if (!create_file(target_parent.value(), name))
	{
		fprintf(stderr, "touch: error creating file\n");
		return;
	}
	sync();
}


/*
Writes buf into path file
*/
void FS::write(std::string path, std::string buf)
{
	if (path.empty()) { fprintf(stderr, "touch: missing operand\n"); return; }
	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "write: invalid path\n"); return; }

	auto target = find_in_dir(target_parent.value(), name);
	if (!target) { fprintf(stderr, "write: file does not exist\n"); return; }

	auto target_meta = read_inode_meta(target.value());
	if (is_dir(target_meta.value())) { fprintf(stderr, "unable to write to directory\n"); return; }
	// write to file

	std::vector<u8> byte_buf(buf.length());

	std::memcpy(byte_buf.data(), buf.data(), byte_buf.size());
	write_inode_data(target.value(), byte_buf);

	sync();
}

void FS::cat(std::string path)
{
	if (path.empty()) { fprintf(stderr, "cat: missing operand\n"); return; }
	std::string full_path = make_path(path);
	auto path_parts = split_path(full_path);

	std::string name;
	auto target_parent = resolve_parent(path_parts, name);
	if (!target_parent) { fprintf(stderr, "invalid path\n"); }

	auto target = find_in_dir(target_parent.value(), name);
	if (!target) { fprintf(stderr, "cat: invalid path\n"); return; }
	auto target_meta = read_inode_meta(target.value());
	if (!target_meta) { fprintf(stderr, "cat: read metadata\n"); return; }

	auto data = read_inode_data(target_meta.value());

	std::string disp_data = "";
	std::memcpy(disp_data.data(), data.value().data(), data.value().size());

	printf("%s\n", disp_data.c_str());

	return;
}
