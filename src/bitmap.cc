#include "bitmap.h"
#include "fs_error.h"

Bitmap::Bitmap(u32 bit_count) : m_bit_count { bit_count }
{
	// one bit per block
	m_size = (bit_count + 7) / 8;
	m_bits.assign(m_size, 0);
}

void Bitmap::set_bit(u32 bit)
{
	u32 index = bit / 8;
	m_bits[index] |= (1u << (bit % 8));
}

void Bitmap::unset_bit(u32 bit)
{
	u32 index = bit / 8;
	m_bits[index] &= ~(1u << (bit % 8));
}

void Bitmap::set_vector(std::vector<u8> bits, size_t bits_used)
{ 
	m_bits = std::move(bits); 
	m_size = m_bits.size();
	m_bit_count = bits_used;
}

std::optional<u32> Bitmap::alloc()
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
	throw FSError(FSErr::NoSpace, "Could not allocate block\n");
} 
