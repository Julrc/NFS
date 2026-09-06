#include <vector>
#include <cstdint>
#include <optional>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class FSErr;
struct FSError;

class Bitmap
{
protected:
	size_t m_bit_count;
	size_t m_size;
	std::vector<u8> m_bits;

public:
	Bitmap() = default;

	Bitmap(u32 bit_count);

	void set_bit(u32 bit);

	void unset_bit(u32 bit);

	std::vector<u8> get_bytes() { return m_bits; }
	void set_vector(std::vector<u8> bits, size_t bits_used);

	// returns offset
	std::optional<u32> alloc();

	size_t get_size() const { return m_size; }
	size_t get_bit_count() const { return m_bit_count; }

};
