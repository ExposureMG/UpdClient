#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace updclient::net {

// A reliable, ordered byte stream. A new transport implements the primitives
// (isOpen, close, describe, setTimeout, readSome, writeSome); the helpers below
// are built on top of them.
class UPDCLIENT_API ITransport {
public:
  ITransport() = default;
  ITransport(const ITransport &) = delete;
  ITransport &operator=(const ITransport &) = delete;
  virtual ~ITransport();

  virtual bool isOpen() const noexcept = 0;
  virtual void close() noexcept = 0;
  virtual std::string describe() const = 0;
  // Zero disables the timeout.
  virtual Result<void> setTimeout(std::chrono::milliseconds timeout) = 0;

  // Returns the number of bytes read; 0 means orderly end of stream.
  virtual Result<size_t> readSome(std::span<uint8_t> buffer) = 0;
  virtual Result<size_t> writeSome(std::span<const uint8_t> data) = 0;

  Result<void> writeAll(std::span<const uint8_t> data);
  Result<void> readExact(std::span<uint8_t> buffer);
  // Reads until orderly EOF. Fails with LimitExceeded if more than maxBytes arrive.
  Result<std::vector<uint8_t>> readUntilEof(size_t maxBytes);
};

using TransportPtr = std::unique_ptr<ITransport>;

} // namespace updclient::net
