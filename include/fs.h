#pragma once

#include <fcntl.h>
#include <cstdint>

#include <unistd.h>

#include <optional>
#include <string>
#include <vector>

#include "bitmap.h"

#define INT_CEIL_DIV(a, b) (((a) + (b) - 1) / (b))

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 KiB(u64 n) { return n << 10; }
constexpr u64 MiB(u64 n) { return n << 20; }
constexpr u64 GiB(u64 n) { return n << 30; }

constexpr u64 BLOCK_SIZE{KiB(4)};
constexpr u32 MAGIC_NUMBER{0xF008DEE8};
constexpr u32 NUM_DIRECT_PTRS{14};
constexpr u32 WRT_BUF_SZ{1024};

struct Super_block_t
{
	u64 sz;
	u32 magic_number;
	u32 block_size;
	u32 inode_count;
	u32 inode_bitmap_offs;
	u32 data_bitmap_offs;
	u32 inode_table_offs;
	u32 data_region_offs;
};

// 128 bytes
struct Inode_t
{
	u32 size; 
	u32 time; // last access
	u32 ctime; // creation time
	u32 mtime; // modified time
	u32 dtime; // deletion time
	u32 blocks; // how many blocks allocated
	u32 flags;
	u32 osd1;
	// set of disk pointers (15 total) 14 direct, 1 indirect
	u32 block_ptrs[15];
	//28 bytes of padding
	u16 mode; // RWE
	u16 uid; // Owner
	u16 gid; // group
	u16 links_count; // hard link reference countfs.h
	u8 type;
	u8 padding[27];
};

struct Entry_t 
{
	u32 inode_num;
	char name[32];
};

struct Path_req_t
{
	bool absolute = false;
	bool must_be_dir = false;
	std::vector<std::string> path_parts;
};

struct Resolved_t
{
	u32 parent{0};
	std::optional<u32> target{0};
	std::string name{""};
	bool must_b_dir{false};
};

class FS
{
private:
	std::string m_name;
	Super_block_t sb;
	Bitmap inode_bitmap;
	Bitmap data_bitmap;
	int m_fd{-1};
	std::vector<std::string> m_cwd_parts;
	u32 m_cwd_inode;

	Path_req_t parse_path(std::string path);
	std::optional<Resolved_t> resolve_path(std::string& raw_path);

	std::optional<std::vector<u8>> read_block(const size_t offset);
	std::optional<Inode_t> read_inode_meta(const u32 inode_num);
	std::optional<std::vector<u8>> read_inode_data(std::optional<Inode_t> inode_meta);

	bool write_block(const size_t offset, const u8 *data, const ssize_t n);
	bool write_inode_meta(u32 inode_num, const Inode_t &metadata);
	bool zero_block(u32 block_offs);
	bool set_block(Inode_t& inode, u32 idx, u32 val);
	std::optional<u32> get_block(Inode_t &inode, u32 idx);
	bool write_inode_data(const u32 inode_num, const std::vector<u8> &data);

	bool flush_bitmap(Bitmap &bm, u32 block_offset);
	bool flush_sb();
	bool sync();

	std::vector<Entry_t> data_parse_dirs(const std::vector<u8>& data_bytes);

	void free_data_block(size_t block) { data_bitmap.unset_bit(block); }
	void free_inode(u32 inode_num) { inode_bitmap.unset_bit(inode_num); }

	std::optional<u32> alloc_data_block();

	std::optional<u32> find_in_dir(u32 dirnode, const std::string &name);
	std::vector<std::string> split_path(std::string path);
	std::optional<u32> resolve_parent(const std::vector<std::string>& parts, std::string& last);

	std::optional<u32> create_dir(u32 inode_num, std::string dir_name);
	bool is_dir(const Inode_t& inode);
	bool is_dir(const u32 inode);
	std::optional<u32> create_file(u32 inode_num, std::string file_name);

	void free_inode_data(Inode_t& target_inode);

	bool free_subtree(u32 inode);
	bool rm_dir(const u32 target_parent, const u32 target);
	bool rm_file(const u32 target_parent, const u32 target);

public:

	FS(std::string name);

	~FS();
	FS(const FS&) = delete;
	FS& operator=(const FS&) = delete;
	FS(FS&& o) = delete;
	FS& operator=(FS&& o) = delete;

	static int mkfs(const char* pathname, u64 sz);

	void mkdir(std::string raw_path);
	void ls(std::string raw_path);
	void cd(std::string raw_path);
	void rm(std::string raw_path);
	void touch(std::string raw_path);
	void write(std::string raw_path, std::string buf);
	void cat(std::string raw_path);

	std::string get_curr_dir();
	void print_superblock();
	void print_bitmaps();
};
