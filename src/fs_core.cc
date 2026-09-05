#include "fs.h"


#include <cstring>

//  TODO: 
//  DOUBLE INDIRECT POINTER
//  OFFSET OVERFLOWS WITH u32
//  REFACTOR CREATE_DIR 


void FS::mkdir(std::string raw_path)
{
	// parse, directories 

	auto path = resolve_path(raw_path);
	if (!path) { fprintf(stderr, "invalid path\n"); return; }

	std::string name = path->name;
	u32 target_parent = path->parent;

	if (find_in_dir(target_parent, name)) { fprintf(stderr, "mkdir: directory already exists\n"); return; }
	if (!create_dir(target_parent, name)) { fprintf(stderr, "mkdir: error creating directory\n"); return; }

	sync();
	// ex. /root/etc
}

void FS::ls(std::string raw_path)
{

	auto path = resolve_path(raw_path);
	if (!path) { fprintf(stderr, "invalid path\n"); return; }

	auto target = path->target; // default for root
	if (!target) { fprintf(stderr, "failed to find target\n"); return; }

	// out put target directories
	
	auto meta = read_inode_meta(target.value());
	if (!meta) { fprintf(stderr, "ls: unable to read directory metadata\n"); return; }
	if (!is_dir(meta.value())) { fprintf(stderr, "invalid, not a directory\n"); }
	auto data = read_inode_data(meta);
	if (!data) { fprintf(stderr, "ls: unable to read directory data\n"); return;}

	std::vector<entry_t> directory_entries = data_parse_dirs(data.value());

	for (auto &entry : directory_entries) { printf("%u, %s\n", entry.inode_num, entry.name); }
}

// from current directory m_curr_dir, traverse to path
// later on save curr dir inode num to remove need to traverse from root
void FS::cd(std::string raw_path)
{
	Path_req_t path = parse_path(raw_path);
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }
	if (!res_path->target) { fprintf(stderr, "no such directory\n"); return; }
	if (!is_dir(res_path->target.value())) { fprintf(stderr, "not a directory\n"); return; };

	// you either cd from an absolute path,  or relative
	
	std::vector<std::string> curr_parts = (path.absolute) ? std::vector<std::string>{}: m_cwd_parts;
	for (auto &p : path.path_parts)
	{
		if (p == "..") { if (!curr_parts.empty()) curr_parts.pop_back(); }
		else { curr_parts.push_back(p); }
	}

	m_cwd_parts = std::move(curr_parts);
	m_cwd_inode = res_path->target.value();
}

void FS::rm(std::string raw_path)
{
	Path_req_t path = parse_path(raw_path);
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "no such directory\n"); return; }

	// refuse if deleting a descendant of current directory
	bool is_descendant{false};
	if (path.absolute)
	{
		if ((path.path_parts.size() < m_cwd_parts.size()) && (std::equal(path.path_parts.begin(), path.path_parts.end(), m_cwd_parts.begin())))
		{
			is_descendant = true;
		}
	}

	if (is_descendant) { fprintf(stderr, "cannt remove descendant directory\n"); return; }

	if (!res_path->target) { fprintf(stderr, "target not found\n"); return; }
	if (res_path->target == 0) { fprintf(stderr, "cannto delete root\n"); return; }
	if (res_path->target == m_cwd_inode) { fprintf(stderr, "cannot remove current direcory\n"); return; }

	// delete target
	auto target_meta = read_inode_meta(res_path->target.value());

	if (is_dir(target_meta.value()))
	{
		if (rm_dir(res_path->parent, res_path->target.value())) { sync(); };
	}
	else
	{
		if (rm_file(res_path->parent, res_path->target.value())) { sync(); };
	}

	return;
}

void FS::touch(std::string raw_path)
{
	if (raw_path.empty()) { fprintf(stderr, "touch: missing operand\n"); return; }

	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }
	if (res_path->must_b_dir) { fprintf(stderr, "touch: arg can not be a directory\n"); return; }

	if (res_path->target) { fprintf(stderr, "file already exists\n"); return; }
	if (!create_file(res_path->parent, res_path->name)) { fprintf(stderr, "touch, error creating file\n"); return; }

	sync();
}


/*
Writes buf into path file
*/
void FS::write(std::string raw_path, std::string buf)
{
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "invalid path\n"); return; }

	if (!res_path->target) { fprintf(stderr, "write: file does not exist\n"); return; }
	if (res_path->must_b_dir) { fprintf(stderr, "write: cannot write to a directory\n"); return; }

	auto target_meta = read_inode_meta(res_path->target.value());
	if (is_dir(target_meta.value())) { fprintf(stderr, "unable to write to directory\n"); return; }
	// write to file

	std::vector<u8> byte_buf(buf.length());

	std::memcpy(byte_buf.data(), buf.data(), byte_buf.size());
	write_inode_data(res_path->target.value(), byte_buf);

	sync();
}

void FS::cat(std::string raw_path)
{
	auto res_path = resolve_path(raw_path);
	if (!res_path) { fprintf(stderr, "cat: invalid path\n"); return; }

	if (!res_path->target) { fprintf(stderr, "file not found\n"); return; }

	if (res_path->must_b_dir) { fprintf(stderr, "cat: cannot use a directory\n"); return; }

	auto target_meta = read_inode_meta(res_path->target.value());
	if (!target_meta) { fprintf(stderr, "cat: read metadata\n"); return; }

	auto data = read_inode_data(target_meta.value());
	if (!data) { fprintf(stderr, "cat: read data\n"); return; }

	std::string disp_data(data->begin(), data->end());
	fwrite(disp_data.data(), 1, disp_data.size(), stdout);
	putchar('\n');

	return;
}
