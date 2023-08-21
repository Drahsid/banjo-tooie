#include <fstream>
#include <fmt/core.h>
#include <fmt/ostream.h>
#include <cstdint>
#include <vector>
#include <bit>
#include <cstring>
#include <unordered_map>
#include <string_view>
#include "zlib.h"

constexpr bool generate_splat_fies = true;

static inline uint32_t byteswap32(uint32_t in) {
    return __builtin_bswap32(in);
}

static inline uint16_t byteswap16(uint16_t in) {
    return __builtin_bswap16(in);
}

enum class RelocType {
    R_MIPS_32 = 0,
    R_MIPS_26 = 1,
    R_MIPS_HI16 = 2,
    R_MIPS_LO16 = 3
};

enum class SymbolType {
    Function,
    Data,
    Rodata,
    Bss,
    None
};

const char* reloc_names[] = {
    "MIPS_32",
    "MIPS_26",
    "MIPS_HI16",
    "MIPS_LO16"
};

struct Symbol {
    std::string ovl_name;
    uint32_t address;

    std::strong_ordering operator<=>(const Symbol& rhs) const {
        return std::tie(ovl_name, address) <=> std::tie(rhs.ovl_name, rhs.address);
    }
    bool operator==(const Symbol&) const = default;
};

struct SymbolDetails {
    uint32_t rom_address;
    SymbolType type;
    std::string name;
};

struct Reloc {
    Symbol symbol;
    uint32_t rom_address;
    RelocType type;
};

struct Overlay {
    std::string name;
    uint32_t rom_start;
    uint32_t code_rom_start;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t rodata_size;
    uint32_t bss_size;
    uint32_t num_entrypoints;
    uint32_t num_relocs;
};

namespace std {
    template <>
    struct hash<Symbol> {
        size_t operator()(const Symbol& s) const {
            std::size_t h1 = std::hash<std::string>{}(s.ovl_name);
            std::size_t h2 = std::hash<uint32_t>{}(s.address);
            return h1 ^ h2;
        }
    };
};

std::string symbol_name(const Symbol& symbol, SymbolType type) {
    const char* type_name;
    switch (type) {
        case SymbolType::Function:
            type_name = "func";
            break;
        case SymbolType::Data:
            type_name = "D";
            break;
        case SymbolType::Rodata:
            type_name = "R";
            break;
        case SymbolType::Bss:
            type_name = "B";
            break;
        default:
            type_name = "undefined";
            break;
    }
    return fmt::format("{}_{:08X}_{}", type_name, symbol.address, symbol.ovl_name);
}

class splat_file_state {
private:
    std::ofstream symbol_addrs_file;
    std::ofstream reloc_addrs_file;
    std::ofstream yaml_file;
    std::unordered_map<Symbol, SymbolDetails> symbols;
    std::vector<Reloc> relocs;
    std::vector<Overlay> overlays;
    size_t overlay_table_rom_start_;
public:
    splat_file_state(size_t overlay_table_rom_start) : overlay_table_rom_start_(overlay_table_rom_start) {
        if constexpr (generate_splat_fies) {
            symbol_addrs_file = std::ofstream{"ovl_symbol_addrs.us.v10.txt"};
            reloc_addrs_file = std::ofstream{"ovl_reloc_addrs.us.v10.txt"};
            yaml_file = std::ofstream{"ovl.us.v10.yaml"};
        }
    }

    void add_reloc(const char* ovl_name, uint32_t symbol_address, uint32_t rom_addr, RelocType reloc_type) {
        if constexpr (generate_splat_fies) {
            relocs.emplace_back(Reloc{.symbol = Symbol{.ovl_name = ovl_name, .address = symbol_address}, .rom_address = rom_addr, .type = reloc_type});
        }
    }

    void add_symbol(const char* ovl_name, uint32_t addr, uint32_t rom_addr) {
        if constexpr (generate_splat_fies) {
            Symbol sym{.ovl_name = ovl_name, .address = addr};
            if (!symbols.contains(sym)) {
                symbols[sym] = SymbolDetails{.rom_address = rom_addr, .type = SymbolType::None, .name = ""};
            }
        }
    }

    void set_symbol_type(const char* ovl_name, uint32_t addr, SymbolType type) {
        symbols[Symbol{.ovl_name = ovl_name, .address = addr}].type = type;
    }

    void add_bss_symbol(const char* ovl_name, uint32_t addr) {
        if constexpr (generate_splat_fies) {
            Symbol sym{.ovl_name = ovl_name, .address = addr};
            if (!symbols.contains(sym)) {
                symbols[sym] = SymbolDetails{0, SymbolType::Bss, ""};
            }
        }
    }

    void print_symbols() {
        if constexpr (generate_splat_fies) {
            for (const auto& [s, details] : symbols) {
                uint32_t rom_addr = details.rom_address;
                const std::string& sym_name = details.name;
                if (rom_addr == 0) {
                    // Bss symbol
                    fmt::print(symbol_addrs_file, "{} = 0x{:08X}; // segment:{}\n",
                        sym_name.empty() ? symbol_name(s, details.type) : sym_name, s.address, s.ovl_name);
                }
                else {
                    // Non-bss symbol
                    fmt::print(symbol_addrs_file, "{} = 0x{:08X}; // segment:{} rom:0x{:08X} {}\n", 
                        sym_name.empty() ? symbol_name(s, details.type) : sym_name, s.address, s.ovl_name, rom_addr, details.type == SymbolType::Function ? "type:func" : "");
                }
            }
            for (const auto& reloc : relocs) {
                const auto& symbol_details = symbols[reloc.symbol];
                const std::string& sym_name = symbol_details.name;
                fmt::print(reloc_addrs_file, "rom:0x{:08X} symbol:{} reloc:{}\n",
                    reloc.rom_address, sym_name.empty() ? symbol_name(reloc.symbol, symbol_details.type) : sym_name, reloc_names[(size_t)reloc.type]);
            }
        }
    }

    void add_overlay(const char* name, uint32_t rom_start, uint32_t code_rom_start, uint32_t text_size, uint32_t data_size, uint32_t rodata_size, uint32_t bss_size, uint32_t num_entrypoints, uint32_t num_relocs) {
        if constexpr (generate_splat_fies) {
            overlays.emplace_back(Overlay{
                .name = name,
                .rom_start = rom_start,
                .code_rom_start = code_rom_start,
                .text_size = text_size,
                .data_size = data_size,
                .rodata_size = rodata_size,
                .bss_size = bss_size,
                .num_entrypoints = num_entrypoints,
                .num_relocs = num_relocs
            });
        }
    }

    void print_overlays() {
        if constexpr (generate_splat_fies) {
            uint32_t rom_end = 0;
            fmt::print(yaml_file,
                "  - name: overlay_table\n"
                "    type: code\n"
                "    vram: 0 # Unimportant dummy value\n"
                "    start: 0x{0:08X}\n"
                "    subsegments: \n"
                "      - [0x{0:08X}, data, overlay_table]\n",
                overlay_table_rom_start_);
            for (const auto& overlay : overlays) {
                uint32_t text_start = overlay.code_rom_start;
                uint32_t data_start = text_start + overlay.text_size;
                uint32_t rodata_start = data_start + overlay.data_size;
                uint32_t rodata_end = rodata_start + overlay.rodata_size;
                fmt::print(yaml_file,
                    "  - name: {0}_header\n"
                    "    dir: {0}\n"
                    "    type: code\n"
                    "    vram: 0 # Unimportant dummy value\n"
                    "    start: 0x{1:08X}\n"
                    "    subsegments: \n"
                    "      - [0x{1:08X}, data, {0}_header]\n"
                    "  - name: {0}\n"
                    "    dir: {0}\n"
                    "    type: code\n"
                    "    vram: 0x80000000\n"
                    "    start: 0x{2:08X}\n"
                    "    subalign: 4\n"
                    "    bss_size: 0x{6:08X}\n"
                    "    subsegments: \n"
                    "      - [0x{2:08X}, c, {0}]\n"
                    "      - [0x{3:08X}, .data, {0}]\n"
                    "      - [0x{4:08X}, .rodata, {0}]\n"
                    "      - [0x{5:08X}, .bss, {0}]\n",
                    overlay.name, overlay.rom_start, text_start, data_start, rodata_start, rodata_end, overlay.bss_size);
                rom_end = rodata_end;
            }
            fmt::print(yaml_file, "  - [0x{:08X}]\n", rom_end);
        }
    }
};

constexpr uint32_t overlay_table_offset = 0x1E899B0;

namespace ovl_header_offsets {
    constexpr size_t text_size = 0x0;
    constexpr size_t data_size = 0x2;
    constexpr size_t rodata_size = 0x4;
    constexpr size_t bss_size = 0x6;
    constexpr size_t entrypoint_count = 0x8;
    constexpr size_t reloc_count = 0xA;
    constexpr size_t flags = 0xC;
    constexpr size_t decompressed_size = 0x10;
};

enum class OverlayFlags {
    Compressed = 0x80,
};

template <typename T>
void resize_if_larger(std::vector<T>& vec, size_t new_size) {
    if (vec.size() < new_size) {
        vec.resize(new_size);
    }
}

void bk_crc(const std::vector<char>& bytes, uint32_t decompressed_size, uint32_t& crc1, uint32_t& crc2) {
    crc1 = 0;
    crc2 = 0;//xFFFFFFFF;
    for (size_t i = 0; i < decompressed_size; i++) {
        uint32_t byte = (uint32_t)(uint8_t)bytes[i];
        
        crc1 += byte;
        crc2 ^= byte << (crc1 & 0x17);
    }
}

char* get_overlay_reloc_ptr(char* overlay_header, char* overlay_contents, uint16_t* num_entrypoints_out, uint16_t* num_relocs_out) {
    // Start by reading the entrypoint and reloc counts from the overlay header
    uint16_t num_entrypoints = byteswap16(*reinterpret_cast<uint16_t*>(overlay_header + ovl_header_offsets::entrypoint_count));
    uint16_t num_relocs = byteswap16(*reinterpret_cast<uint16_t*>(overlay_header + ovl_header_offsets::reloc_count));

    // Offset the overlay contents by 0x28 to get to the entrypoints
    char* ovl_entrypoints = overlay_contents + 0x28;
    // Offset the entrypoints by [entrypoint count] 32-bit values
    char* ovl_name = ovl_entrypoints + sizeof(uint32_t) * num_entrypoints;
    // Determine the length of the overlay name
    size_t name_length = std::strlen(ovl_name) + 1; // include the null terminator in the length
    // Round the overlay name up to the nearest 4 bytes and offset from the name start to get to the relocs
    char* ovl_relocs = ovl_name + ((name_length + 4 - 1) & -4);

    *num_entrypoints_out = num_entrypoints;
    *num_relocs_out = num_relocs;
    return ovl_relocs;
}

void undo_xors(size_t overlay_index, char* rom_start, std::vector<char>& overlay_data, std::vector<char>& decompressed_buffer, size_t decompressed_size) {
    // Calculate the CRCs of the decompressed overlay data
    uint32_t crc1, crc2;
    bk_crc(decompressed_buffer, decompressed_size, crc1, crc2);
    // printf("crcs: %08X %08X\n", crc1, crc2);

    // Reverse the CRC xor in the overlay
    *reinterpret_cast<uint32_t*>(overlay_data.data() + 0) ^= byteswap32(crc1);
    *reinterpret_cast<uint32_t*>(overlay_data.data() + 8) ^= byteswap32(crc2);

    // Get the pointer to the relocs and the number of relocs
    uint16_t num_relocs;
    uint16_t num_entrypoints;
    char* ovl_relocs = get_overlay_reloc_ptr(overlay_data.data(), decompressed_buffer.data(), &num_entrypoints, &num_relocs);

    // Read the data from the start of the rom used to xor the relocs
    uint32_t rom_xor = byteswap32(*reinterpret_cast<uint32_t*>(rom_start + 0x40 + overlay_index * sizeof(uint32_t))) & 0xFFFF;
    uint32_t rom_xor_swapped = byteswap16(rom_xor);

    // Reverse the reloc xors in the overlay (use the byteswapped xor value to avoid needing to byteswap the reloc value)
    for (uint16_t reloc_index = 0; reloc_index < num_relocs; reloc_index++) {
        *reinterpret_cast<uint16_t*>(ovl_relocs + reloc_index * sizeof(uint16_t)) ^= rom_xor_swapped;
    }
}

void parse_relocs(splat_file_state& splat_files, uint32_t ovl_rom_address, const char* ovl_name, char* overlay_header, char* overlay_contents, uint32_t text_size, uint32_t data_size, uint32_t rodata_size, uint32_t bss_size) {
    // Get the pointer to the relocs and the number of relocs
    uint16_t num_entrypoints;
    uint16_t num_relocs;
    char* ovl_relocs = get_overlay_reloc_ptr(overlay_header, overlay_contents, &num_entrypoints, &num_relocs);
    char* ovl_relocs_end = ovl_relocs + num_relocs * sizeof(uint16_t);
    size_t ovl_relocs_end_offset = ovl_relocs_end - overlay_contents;
    // Align the overlay offset to 16 bytes to get the location of code
    size_t ovl_code_offset = ((ovl_relocs_end_offset + 16 - 1) & -16);
    char* ovl_code = overlay_contents + ovl_code_offset;

    uint32_t text_end = 0x80000000 + text_size;
    uint32_t data_end = text_end + data_size;
    uint32_t rodata_end = data_end + rodata_size;
    uint32_t bss_end = rodata_end + bss_size;

    fmt::print(" Text {:08X} Data {:08X} Rodata {:08X} Bss {:08X}\n", text_size, data_size, rodata_size, bss_size);
    fmt::print(" Text {0:08X} - {1:08X}, Data {1:08X} - {2:08X}, Rodata {2:08X} - {3:08X}, Bss {3:08X} - 0x{4:08X}\n",
        0x80000000, text_end, data_end, rodata_end, bss_end);
    printf("  code offset: 0x%08lX\n", ovl_code_offset);

    bool tracking_hi = false;
    uint32_t prev_hi_addend = 0;
    for (uint16_t reloc_index = 0; reloc_index < num_relocs; reloc_index++) {
        // Read the reloc value
        uint16_t reloc_val = byteswap16(*reinterpret_cast<uint16_t*>(ovl_relocs + reloc_index * sizeof(uint16_t)));
        // Determine the reloc offset and type
        RelocType reloc_type = (RelocType)(reloc_val & 0x3);
        uint16_t reloc_offset = reloc_val & ~0x3;
        // Get the overlay word that the reloc corresponds to
        uint32_t reloc_word = byteswap32(*reinterpret_cast<uint32_t*>(ovl_code + reloc_offset));
        printf("    %4d Type: %-11s Offset: 0x%04X Word: %08X (Val: 0x%04X)\n", reloc_index, reloc_names[(size_t)reloc_type], reloc_offset, reloc_word, reloc_val);

        uint32_t reloc_addend = 0;

        if (reloc_type == RelocType::R_MIPS_HI16) {
            reloc_addend = (reloc_word & 0xFFFF) << 16;
            prev_hi_addend = reloc_addend;
            tracking_hi = true;
            // Get the full addend
            // This wouldn't be needed for just relocating this instruction, but it is for generating a symbol name for splat
            if (reloc_index + 1 < num_relocs) {
                // Read the next reloc value
                uint16_t next_reloc_val = byteswap16(*reinterpret_cast<uint16_t*>(ovl_relocs + (reloc_index + 1) * sizeof(uint16_t)));
                // Determine the next reloc offset and type
                RelocType next_reloc_type = (RelocType)(next_reloc_val & 0x3);
                uint16_t next_reloc_offset = next_reloc_val & ~0x3;
                // Get the overlay word that the next reloc corresponds to
                uint32_t next_reloc_word = byteswap32(*reinterpret_cast<uint32_t*>(ovl_code + next_reloc_offset));
                if (next_reloc_type == RelocType::R_MIPS_LO16) {
                    reloc_addend += (int16_t)(next_reloc_word & 0xFFFF);
                }
                else {
                    fprintf(stderr, "hi with wrong reloc type afterwards (%s)\n", reloc_names[(size_t)next_reloc_type]);
                }
            }
            else {
                fprintf(stderr, "hi with no reloc afterwards\n");
            }
        }
        else if (reloc_type == RelocType::R_MIPS_LO16) {
            if (tracking_hi) {
                reloc_addend = prev_hi_addend + (int16_t)(reloc_word & 0xFFFF);
            }
            else {
                fprintf(stderr, "lo without hi (reloc %d in %s)\n", reloc_index, ovl_name);
                reloc_addend = (int16_t)(reloc_word & 0xFFFF);
            }
        }
        else if (reloc_type == RelocType::R_MIPS_32) {
            reloc_addend = reloc_word;
        }
        else if (reloc_type == RelocType::R_MIPS_26) {
            reloc_addend = (reloc_word & ((1 << 26) - 1)) << 2;
        }

        // Stop tracking the previous HI16 if we see anything other than a HI16 or LO16
        if (reloc_type != RelocType::R_MIPS_HI16 && reloc_type != RelocType::R_MIPS_LO16) {
            tracking_hi = false;
        }

        uint32_t symbol_address = 0x80000000 + reloc_addend;
        fmt::print("      0x{:08X}\n", symbol_address);
        
        if (symbol_address >= rodata_end) {
            printf("      bss\n");
            splat_files.add_bss_symbol(ovl_name, symbol_address);
        }
        else {
            splat_files.add_symbol(ovl_name, symbol_address, ovl_rom_address + 0x10 + ovl_code_offset + reloc_addend);
        }
        splat_files.add_reloc(ovl_name, symbol_address, ovl_rom_address + 0x10 + ovl_code_offset + reloc_offset, reloc_type);

        if (symbol_address < text_end) {
            splat_files.set_symbol_type(ovl_name, symbol_address, SymbolType::Function);
        }
        else if (symbol_address < data_end) {
            splat_files.set_symbol_type(ovl_name, symbol_address, SymbolType::Data);
        }
        else if (symbol_address < rodata_end) {
            splat_files.set_symbol_type(ovl_name, symbol_address, SymbolType::Rodata);
        }
        else if (symbol_address < bss_end) {
            splat_files.set_symbol_type(ovl_name, symbol_address, SymbolType::Rodata);
        }
        else {
            fmt::print(stderr, "Symbol in reloc {} in {} is beyond the end of bss\n", reloc_index, ovl_name);
        }
    }
}

std::vector<char> read_file_contents(const char* file_path) {
    std::vector<char> ret;

    // Open the file    
    std::ifstream input_file{file_path, std::ios::binary};

    // Get the file's length
    input_file.seekg(0, std::ios::end);
    size_t file_size = input_file.tellg();

    // Seek back to the start and read the file's contents
    input_file.seekg(0);
    ret.resize(file_size);
    input_file.read(ret.data(), file_size);

    return ret;
}

void write_partial_contents(std::ofstream& output_rom_file, const char* partial_decompressed_rom_path) {
    // Read the contents of the partial decompressed rom
    std::vector<char> partial_rom_data = read_file_contents(partial_decompressed_rom_path);

    // Write them to the output rom
    output_rom_file.write(partial_rom_data.data(), partial_rom_data.size());
}

int main(int argc, const char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Decompresses banjo tooie overlays and adds them to the end of a\n"
                        "partially decompressed rom. Meant to be used in conjunction with\n"
                        "bk rom decompressor.\n");
        fprintf(stderr, "Usage: %s [baserom] [partially decompressed rom path] [output rom path]\n", argv[0]);
        return EXIT_SUCCESS;
    }

    const char* baserom_path = argv[1];
    const char* partial_rom_path = argv[2];
    const char* output_rom_path = argv[3];

    std::ofstream output_rom_file{output_rom_path, std::ios::binary};
    write_partial_contents(output_rom_file, partial_rom_path);

    char rom_start[0x1000];
    std::ifstream rom_file{baserom_path, std::ios::binary};

    splat_file_state splat_files{(size_t)output_rom_file.tellp()};
    
    rom_file.seekg(0);
    rom_file.read(rom_start, sizeof(rom_start));

    rom_file.seekg(overlay_table_offset);

    uint32_t overlay_table_size;
    rom_file.read(reinterpret_cast<char*>(&overlay_table_size), sizeof(overlay_table_size));
    overlay_table_size = byteswap32(overlay_table_size);
    
    // Record the current output file position to know where to write the overlay table later
    size_t cur_output_pos = output_rom_file.tellp();
    // Align the overlay table to 16 bytes like in the compressed rom
    size_t output_overlay_table_pos = (cur_output_pos + 15) & -16;
    // Pad with zeroes to read the alignment
    static const char zeroes[]  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    output_rom_file.write(zeroes, output_overlay_table_pos - cur_output_pos);

    size_t overlay_count = overlay_table_size / sizeof(uint32_t) - 1;
    std::vector<uint32_t> overlay_table;
    overlay_table.resize(overlay_count + 1);

    rom_file.seekg(overlay_table_offset);
    rom_file.read(reinterpret_cast<char*>(overlay_table.data()), (overlay_count + 1) * sizeof(uint32_t));

    for (auto& overlay_table_entry : overlay_table) {
        overlay_table_entry = byteswap32(overlay_table_entry);
    }

    std::vector<char> overlay_header;
    std::vector<char> decompressed_buffer;
    overlay_header.resize(512);
    decompressed_buffer.resize(1024);

    size_t total_compressed = 0;
    size_t total_decompressed = 0;

    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;
    inflateInit2(&stream, -MAX_WBITS);

    // Seek past the overlay table since that'll be written afterwards
    output_rom_file.seekp((overlay_count + 1) * sizeof(uint32_t), std::ios::cur);
    
    // Track the file offsets of each overlay as they're written
    std::vector<uint32_t> byteswapped_overlay_offsets;
    byteswapped_overlay_offsets.resize(overlay_count + 1);

    // Overlays are 1-indexed, so start at 1 and include the count in iteration
    for (size_t overlay_index = 1; overlay_index <= overlay_count; overlay_index++) {
        byteswapped_overlay_offsets[overlay_index - 1] = byteswap32((uint32_t)output_rom_file.tellp() - output_overlay_table_pos);

        uint32_t overlay_start = overlay_table[overlay_index - 1];
        uint32_t overlay_end   = overlay_table[overlay_index];
        printf("Ovl %lu: 0x%04X bytes\n", overlay_index, overlay_end - overlay_start);
        char* overlay_contents;
        size_t overlay_contents_length;

        if (overlay_end != overlay_start) {
            resize_if_larger(overlay_header, overlay_end - overlay_start);
            rom_file.seekg(overlay_start + overlay_table_offset);
            rom_file.read(overlay_header.data(), overlay_end - overlay_start);

            uint32_t flags = byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + ovl_header_offsets::flags));
            // printf("  sp3C: 0x%04X Flags: 0x%02X\n", sp3C, flags);

            if (flags & (uint32_t)OverlayFlags::Compressed) {
                uint32_t decompressed_size = byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + ovl_header_offsets::decompressed_size));
                flags &= ~(uint32_t)OverlayFlags::Compressed;
                *reinterpret_cast<uint32_t*>(overlay_header.data() + ovl_header_offsets::flags) = byteswap32(flags);

                decompressed_size = ((decompressed_size & 0xFFFF0000) >> 0x10) * 0x10;
                printf("  Compressed, decompressed size: 0x%04X\n", decompressed_size);
                total_compressed += overlay_end - overlay_start;
                total_decompressed += decompressed_size;

                resize_if_larger(decompressed_buffer, decompressed_size);

                constexpr int header_size = 0x12;
                stream.avail_in = overlay_end - overlay_start - header_size;
                stream.next_in = reinterpret_cast<Bytef*>(overlay_header.data() + header_size);

                stream.avail_out = decompressed_size;
                stream.next_out = reinterpret_cast<Bytef*>(decompressed_buffer.data());
                int err = 0;
                while (!err) {
                    err = inflate(&stream, Z_NO_FLUSH);
                }
                if (err != Z_STREAM_END) {
                    printf("Zlib error: %d\n", err);
                    return err;
                }
                inflateReset(&stream);

                undo_xors(overlay_index, rom_start, overlay_header, decompressed_buffer, decompressed_size);

                // printf("  %08X %08X %08X %08X\n",
                //     byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 0)),
                //     byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 4)),
                //     byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 8)),
                //     byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 12)));
                // for (int i = 0; i < 15; i++) {
                //     printf("  %08X %08X %08X %08X\n",
                //         byteswap32(*reinterpret_cast<uint32_t*>(decompressed_buffer.data() + 16 * i + 0)),
                //         byteswap32(*reinterpret_cast<uint32_t*>(decompressed_buffer.data() + 16 * i + 4)),
                //         byteswap32(*reinterpret_cast<uint32_t*>(decompressed_buffer.data() + 16 * i + 8)),
                //         byteswap32(*reinterpret_cast<uint32_t*>(decompressed_buffer.data() + 16 * i + 12)));
                // }
                overlay_contents = decompressed_buffer.data();
                overlay_contents_length = decompressed_size;
            } else {
                overlay_contents = overlay_header.data() + 0x10;
                overlay_contents_length = overlay_header.size() - 0x10;
                printf("  Uncompressed\n");
            }
            
            uint32_t text_size = 16 * byteswap16(*reinterpret_cast<uint16_t*>(overlay_header.data() + ovl_header_offsets::text_size));
            uint32_t data_size = 16 * byteswap16(*reinterpret_cast<uint16_t*>(overlay_header.data() + ovl_header_offsets::data_size));
            uint32_t rodata_size = 16 * byteswap16(*reinterpret_cast<uint16_t*>(overlay_header.data() + ovl_header_offsets::rodata_size));
            uint32_t bss_size = 16 * byteswap16(*reinterpret_cast<uint16_t*>(overlay_header.data() + ovl_header_offsets::bss_size));
            fmt::print(" Parsed Text {:08X} Data {:08X} Rodata {:08X} Bss {:08X}\n", text_size, data_size, rodata_size, bss_size);

            // Get the pointer to the relocs and the number of relocs
            uint16_t num_relocs;
            uint16_t num_entrypoints;
            char* ovl_relocs = get_overlay_reloc_ptr(overlay_header.data(), overlay_contents, &num_entrypoints, &num_relocs);
            char* ovl_relocs_end = ovl_relocs + num_relocs * sizeof(uint16_t);
            size_t ovl_relocs_end_offset = ovl_relocs_end - overlay_contents;
            // Align the overlay offset to 16 bytes to get the location of code
            size_t ovl_code_offset = ((ovl_relocs_end_offset + 16 - 1) & -16);
            const char* ovl_name = overlay_contents + num_entrypoints * sizeof(uint32_t) + 0x28;
            printf("  Name: %s\n", ovl_name);

            size_t before = output_rom_file.tellp();
            parse_relocs(splat_files, before, ovl_name, overlay_header.data(), overlay_contents, text_size, data_size, rodata_size, bss_size);

            size_t pad_amount = 16 - (before & (16 - 1));

            if (pad_amount != 16) {
                output_rom_file.write(zeroes, pad_amount);
                before = output_rom_file.tellp();
                byteswapped_overlay_offsets[overlay_index - 1] = byteswap32((uint32_t)before - output_overlay_table_pos);
            }

            output_rom_file.write(overlay_header.data(), 0x10);
            output_rom_file.write(overlay_contents, overlay_contents_length);

            size_t after = output_rom_file.tellp();

            printf("  %08X %08X %08X %08X\n",
                byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 0)),
                byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 4)),
                byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 8)),
                byteswap32(*reinterpret_cast<uint32_t*>(overlay_header.data() + 12)));

            printf("  Code Size: 0x%08lX Total Size: 0x%08lX From: 0x%08lX to 0x%08lX\n", after - before - ovl_code_offset - 0x10, after - before, before, after);
            printf("  Num Entrypoints: %u\n", num_entrypoints);
            splat_files.add_overlay(ovl_name, before, before + ovl_code_offset + 0x10, text_size, data_size, rodata_size, bss_size, num_entrypoints, num_relocs);
        } else {
            printf("  Empty overlay\n");
        }
    }

    splat_files.print_symbols();
    splat_files.print_overlays();

    printf("Compression ratio: %5.3f\n", (float)total_compressed / total_decompressed);
    byteswapped_overlay_offsets[overlay_count] = byteswap32((uint32_t)output_rom_file.tellp() - output_overlay_table_pos);

    // Overlay 0 is empty, so make its start equal the next overlay's start in case any padding was added after the overlay table
    byteswapped_overlay_offsets[0] = byteswapped_overlay_offsets[1];

    // Rewind the output rom file and write the overlay table
    output_rom_file.seekp(output_overlay_table_pos);
    output_rom_file.write(reinterpret_cast<char*>(byteswapped_overlay_offsets.data()), byteswapped_overlay_offsets.size() * sizeof(byteswapped_overlay_offsets[0]));

    return 0;
}