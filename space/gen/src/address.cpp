// veach: canonical
#include <veach/space/gen/address.hpp>

#include <charconv>
#include <format>

namespace space::gen {

namespace {

constexpr std::uint64_t mask(std::uint32_t bits) {
    return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1u;
}

}  // namespace

std::optional<SystemAddress> SystemAddress::encode(const SystemCoords& c) {
    if (c.layer >= address_layers) {
        return std::nullopt;
    }
    const std::uint32_t xz = cell_bits_xz(c.layer), yb = cell_bits_y(c.layer), sb = system_bits(c.layer);
    if (c.x > mask(xz) || c.z > mask(xz) || c.y > mask(yb) || c.system > mask(sb)) {
        return std::nullopt;
    }
    std::uint64_t v = std::uint64_t{c.layer};
    v = (v << xz) | c.x;
    v = (v << xz) | c.z;
    v = (v << yb) | c.y;
    v = (v << sb) | c.system;
    v <<= body_bits;
    return SystemAddress{v};
}

SystemCoords SystemAddress::decode() const {
    SystemCoords c;
    c.layer = static_cast<std::uint32_t>(value >> 61);
    const std::uint32_t xz = cell_bits_xz(c.layer), yb = cell_bits_y(c.layer), sb = system_bits(c.layer);
    std::uint64_t v = value >> body_bits;
    c.system = v & mask(sb);
    v >>= sb;
    c.y = static_cast<std::uint32_t>(v & mask(yb));
    v >>= yb;
    c.z = static_cast<std::uint32_t>(v & mask(xz));
    v >>= xz;
    c.x = static_cast<std::uint32_t>(v & mask(xz));
    return c;
}

BodyAddress SystemAddress::body(std::uint32_t index) const {
    return BodyAddress{value | (std::uint64_t{index} & mask(body_bits))};
}

double SystemAddress::cell_edge_ly() const {
    return cell_edge_ly_layer0 * static_cast<double>(std::uint64_t{1} << decode().layer);
}

void SystemAddress::cell_origin_ly(double out[3]) const {
    const SystemCoords c = decode();
    const double edge = cell_edge_ly();
    const auto half_xz = static_cast<double>(std::uint64_t{1} << (cell_bits_xz(c.layer) - 1u));
    const auto half_y = static_cast<double>(std::uint64_t{1} << (cell_bits_y(c.layer) - 1u));
    out[0] = (static_cast<double>(c.x) - half_xz) * edge;
    out[1] = (static_cast<double>(c.y) - half_y) * edge;
    out[2] = (static_cast<double>(c.z) - half_xz) * edge;
}

std::string to_string(SystemAddress a) {
    return std::format("S:{:016x}", a.value);
}

std::string to_string(BodyAddress a) {
    return std::format("S:{:016x}/B:{}", a.system().value, a.index());
}

std::optional<SystemAddress> parse_system_address(std::string_view text) {
    if (text.size() != 18 || !text.starts_with("S:")) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data() + 2, text.data() + text.size(), v, 16);
    if (ec != std::errc{} || end != text.data() + text.size() || (v & mask(body_bits)) != 0) {
        return std::nullopt;
    }
    return SystemAddress{v};
}

}  // namespace space::gen
