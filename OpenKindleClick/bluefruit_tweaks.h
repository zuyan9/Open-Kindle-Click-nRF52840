#pragma once

// Settings the Bluefruit library has no API for.

// Stop handing our IRK and identity address to the central during pairing.
// This firmware always advertises with its static address, so a central that
// stored an IRK for us could put us in its controller resolving list in
// Network privacy mode and then ignore our (non-resolvable) advertising.
// Without an IRK the central simply reconnects to the static address.
void bluefruitDontDistributeIdentityKey();
