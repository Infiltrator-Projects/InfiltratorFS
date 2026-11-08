// SPDX-License-Identifier: GPL-3.0-or-later
// Retired Mintstick integration guard. Deliberately always fails closed.
// Mintstick repartitions whole devices and is not a safe partition formatter.
#include <cstdio>
int main() {
    std::fputs("InfiltratorFS: Mintstick integration is disabled because Mintstick "
               "repartitions whole devices. Use InfiltratorFS Manager or the "
               "libblockdev/GNOME Disks partition formatter instead.\n",stderr);
    return 2;
}
