#include <bluefruit.h>

#include "bluefruit_tweaks.h"

// BLESecurity::_sec_param is private. Explicit template instantiation may name
// private members ([temp.explicit]), which reaches it without patching the
// library.
namespace {
template <typename Tag, typename Tag::type Member>
struct Grant {
  friend typename Tag::type get(Tag) { return Member; }
};

struct SecParams {
  typedef ble_gap_sec_params_t BLESecurity::*type;
  friend type get(SecParams);
};

template struct Grant<SecParams, &BLESecurity::_sec_param>;
}  // namespace

void bluefruitDontDistributeIdentityKey() {
  ble_gap_sec_params_t& params = Bluefruit.Security.*get(SecParams());
  params.kdist_own.id = 0;  // keep kdist_own.enc (legacy pairing) and kdist_peer
}
