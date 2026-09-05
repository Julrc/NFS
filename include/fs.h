#pragma once

#include <fcntl.h>
#include <cstdint>

#include <unistd.h>

#include <optional>
#include <string>
#include <vector>
#include <utility>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 MAGIC_NUMBER{0xF008DEE8};
constexpr u32 NUM_DIRECT_PTRS{15};
constexpr u32 WRT_BUF_SZ{1024};

struct super_block_t
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
struct inode_t
{
	u32 size; 
	u32 time; // last access
	u32 ctime; // creation time
	u32 mtime; // modified time
	u32 dtime; // deletion time
	u32 blocks; // how many blocks allocated
	u32 flags;
	u32 osd1;
	// set of disk pointers (15 total)
	u32 block_ptrs[15];
	//28 bytes of padding
	u16 mode; // RWE
	u16 uid; // Owner
	u16 gid; // group
	u16 links_count; // hard link reference count
	u8 type;
	u8 padding[27];
};

struct entry_t 
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

class Bitmap
{
protected:
	size_t m_bit_count;
	size_t m_size;
	std::vector<u8> m_bits;

public:
	Bitmap() = default;

	Bitmap(u32 bit_count) : m_bit_count { bit_count }
	{
		// one bit per block
		m_size = (bit_count + 7) / 8;
		m_bits.assign(m_size, 0);
	}

	void set_bit(u32 bit)
	{
		u32 index = bit / 8;
		m_bits[index] |= (1u << (bit % 8));
	}

	void unset_bit(u32 bit)
	{
		u32 index = bit / 8;
		m_bits[index] &= ~(1u << (bit % 8));
	}

	std::vector<u8> get_bytes() { return m_bits; }
	void set_vector(std::vector<u8> bits, size_t bits_used)
	{ 
		m_bits = std::move(bits); 
		m_size = m_bits.size();
		m_bit_count = bits_used;
	}

	// returns offset
	std::optional<u32> alloc()
	{
		// iterate for each byte
		for (size_t i = 0; i < m_size; ++i)
		{
			// iterate for each bit
			for (size_t j = 0; j < 8; ++j)
			{
			if (!(m_bits[i] & (1u << j)))
				{ 
					size_t idx = i * 8 + j;
					if (idx >= m_bit_count) { return std::nullopt; }
					set_bit(idx);
					return static_cast<u32>(idx);
				}
			}
		}
		return std::nullopt;
	} 

	size_t get_size() const { return m_size; }
	size_t get_bit_count() const { return m_bit_count; }

};

class FS
{
private:
	std::string m_name;
	super_block_t sb;
	Bitmap inode_bitmap;
	Bitmap data_bitmap;
	int m_fd{-1};
	std::vector<std::string> m_cwd_parts;
	u32 m_cwd_inode;

	Path_req_t parse_path(std::string path);
	std::optional<Resolved_t> resolve_path(std::string& raw_path);

	std::optional<std::vector<u8>> read_block(const size_t offset);
	std::optional<inode_t> read_inode_meta(const u32 inode_num);
	std::optional<std::vector<u8>> read_inode_data(std::optional<inode_t> inode_meta);


	bool write_block(const size_t offset, const u8 *data, const int n);
	bool write_inode_meta(u32 inode_num, const inode_t &metadata);
	u32 get_block(inode_t &inode, u32 idx, bool rd_mode);
	bool write_inode_data(const u32 inode_num, const std::vector<u8> &data);

	bool flush_bitmap(Bitmap &bm, u32 block_offset);
	bool flush_sb();
	bool sync();

	std::vector<entry_t> data_parse_dirs(const std::vector<u8>& data_bytes);

	void free_data_block(size_t block) { data_bitmap.unset_bit(block); }
	void free_inode(u32 inode_num) { inode_bitmap.unset_bit(inode_num); }

	std::optional<u32> alloc_data_block()
	{ 
		std::optional<u32> idx = data_bitmap.alloc(); 
		if (!idx) return std::nullopt;
		data_bitmap.set_bit(idx.value());
		return data_bitmap.alloc();
	}

	std::optional<u32> find_in_dir(u32 dirnode, const std::string &name);
	std::vector<std::string> split_path(std::string path);
	std::optional<u32> resolve_parent(const std::vector<std::string>& parts, std::string& last);

	std::optional<u32> create_dir(u32 inode_num, std::string dir_name);
	bool is_dir(const inode_t& inode);
	bool is_dir(const u32 inode);
	std::optional<u32> create_file(u32 inode_num, std::string file_name);

	void free_inode_data(inode_t& target_inode);

	bool free_subtree(u32 inode);
	bool rm_dir(const u32 target_parent, const u32 target);
	bool rm_file(const u32 target_parent, const u32 target);

public:
	FS(std::string name) : m_name{ std::move(name) }, m_cwd_inode{ 0 }
	{
		// read superblock (mount logic, resource acquisition)
		int fd = open(m_name.c_str(), O_RDWR);
		if (fd < 0) { perror("openfs: open"); return; }
		m_fd = fd;

		//read superblock and verify magic number
		ssize_t bytes_read_sb = pread(fd, &sb, sizeof(sb), 0);
		if (bytes_read_sb < 0 || (size_t)bytes_read_sb != sizeof(sb))
		{
			fprintf(stderr, "Error reading superblock\n");
			return;
		}

		if (sb.magic_number != MAGIC_NUMBER)
		{
			fprintf(stderr, "FS does not match: bad magic number\n");
			return;
		}

		u32 inode_bitmap_sz = (sb.inode_count + 7) / 8;
		std::vector<u8> inode_bitmap_vec(inode_bitmap_sz);
		ssize_t bytes_read_ib = pread(fd, inode_bitmap_vec.data(), inode_bitmap_vec.size(), sb.inode_bitmap_offs * sb.block_size);
		if (bytes_read_ib < 0 || ((size_t)bytes_read_ib != inode_bitmap_vec.size()))
		{
			fprintf(stderr, "Error reading inode bitmap\n");
			return;
		}

		inode_bitmap.set_vector(inode_bitmap_vec, sb.inode_count);

		u32 fs_block_count = sb.sz / sb.block_size;
		u32 data_bitmap_sz = (fs_block_count + 7) / 8;
		std::vector<u8> data_bitmap_vec(data_bitmap_sz);

		ssize_t bytes_read_db = pread(fd, data_bitmap_vec.data(), data_bitmap_vec.size(), sb.data_bitmap_offs * sb.block_size);
		if (bytes_read_db < 0 || ((size_t)bytes_read_db != data_bitmap_vec.size()))
		{
			fprintf(stderr, "Error reading data bitmap\n");
			return;
		}
		data_bitmap.set_vector(data_bitmap_vec, fs_block_count);
	}

	~FS()
	{
		if (m_fd >= 0) { close(m_fd); }
	}
	FS(const FS&) = delete;
	FS& operator=(const FS&) = delete;
	FS(FS&& o) = delete;
	FS& operator=(FS&& o) = delete;

	void mkdir(std::string raw_path);
	void ls(std::string raw_path);
	void cd(std::string raw_path);
	void rm(std::string raw_path);
	void touch(std::string raw_path);
	void write(std::string raw_path, std::string buf);
	void cat(std::string raw_path);

	std::string get_curr_dir() 
	{ 
		std::string curr_dir = "/";
		for (auto &p : m_cwd_parts)
		{
			curr_dir += p;
			curr_dir += "/";
		}
		return curr_dir;
	}
	void print_superblock();
	void print_bitmaps();
};
