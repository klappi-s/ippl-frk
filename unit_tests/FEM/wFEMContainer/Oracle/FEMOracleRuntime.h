#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "FEMOracleData.h"

namespace fem_oracle::runtime {

    // Host storage only. Existing test fixtures perform the host/device transfers.
    // The package's SHA-256 checks belong to CMake; this reader validates its layout.
    class BinaryArrays {
        std::vector<double> reals_m;
        std::vector<std::size_t> indices_m;

        static void read(std::ifstream& stream, void* destination, std::size_t size) {
            if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                throw std::runtime_error("FEM oracle read exceeds stream size");
            if (size && !stream.read(static_cast<char*>(destination), size))
                throw std::runtime_error("Truncated FEM oracle data");
        }
        template <typename T>
        static T littleEndian(T value) {
            if constexpr (std::endian::native == std::endian::big) {
                auto bytes = std::bit_cast<std::array<unsigned char, sizeof(T)>>(value);
                std::reverse(bytes.begin(), bytes.end());
                return std::bit_cast<T>(bytes);
            }
            return value;
        }
        template <typename T>
        static T scalar(std::ifstream& stream) {
            T value{};
            read(stream, &value, sizeof(value));
            return littleEndian(value);
        }

    public:
        explicit BinaryArrays(const std::filesystem::path& path, unsigned dimension) {
            static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
            static_assert(std::endian::native == std::endian::little
                          || std::endian::native == std::endian::big);
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                throw std::runtime_error("Cannot open FEM oracle data: " + path.string());
            std::array<char, 8> magic{};
            read(stream, magic.data(), magic.size());
            if (magic != std::array<char, 8>{'I', 'P', 'P', 'L', 'F', 'E', 'M', '\0'}
                || scalar<std::uint32_t>(stream) != 1 || scalar<std::uint32_t>(stream) != dimension)
                throw std::runtime_error("Unsupported FEM oracle format or dimension");
            const auto nReals   = scalar<std::uint64_t>(stream);
            const auto nIndices = scalar<std::uint64_t>(stream);
            const auto bytes    = std::filesystem::file_size(path);
            if (bytes < 32 || nReals > (bytes - 32) / 8 || nIndices > (bytes - 32 - nReals * 8) / 4
                || bytes != 32 + nReals * 8 + nIndices * 4 || nReals > reals_m.max_size()
                || nIndices > indices_m.max_size())
                throw std::runtime_error("Invalid FEM oracle array sizes");
            reals_m.resize(static_cast<std::size_t>(nReals));
            read(stream, reals_m.data(), reals_m.size() * sizeof(double));
            for (auto& value : reals_m)
                value = littleEndian(value);
            std::vector<std::uint32_t> packedIndices(static_cast<std::size_t>(nIndices));
            read(stream, packedIndices.data(), packedIndices.size() * sizeof(std::uint32_t));
            indices_m.reserve(packedIndices.size());
            for (auto value : packedIndices)
                indices_m.push_back(littleEndian(value));
        }
        BinaryArrays(const BinaryArrays&)            = delete;
        BinaryArrays& operator=(const BinaryArrays&) = delete;

        Reals reals(std::size_t offset, std::size_t count) const {
            if (offset > reals_m.size() || count > reals_m.size() - offset)
                throw std::runtime_error("FEM oracle real span exceeds array storage");
            return std::span<const double>(reals_m).subspan(offset, count);
        }
        Indices indices(std::size_t offset, std::size_t count) const {
            if (offset > indices_m.size() || count > indices_m.size() - offset)
                throw std::runtime_error("FEM oracle index span exceeds array storage");
            return std::span<const std::size_t>(indices_m).subspan(offset, count);
        }
    };
}  // namespace fem_oracle::runtime
