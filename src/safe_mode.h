#ifndef PITROVE_SAFE_MODE_H
#define PITROVE_SAFE_MODE_H

namespace pitrove { namespace safe_mode {

void record_crash();
bool should_enter_safe_mode();
void clear();

}} // namespace pitrove::safe_mode

#endif // PITROVE_SAFE_MODE_H
