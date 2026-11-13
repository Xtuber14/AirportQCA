#!/usr/bin/env python3
"""Unit test for simulate.patch_vtable rule logic."""
import sys
import simulate


class FakeKext:
    def __init__(self, slots, local_defined=(), undefined=()):
        self._slots = slots
        self.local_defined = set(local_defined)
        self.undefined = set(undefined)
        self.local_by_value = {}

    def vtable_slots(self, name):
        return self._slots


def mk(child_name, parent_name):
    """Child slot referring to child_name; parent entry at the same index."""
    slots = [(0, child_name), (0, None)]     # one method slot + terminator
    parent = {0: parent_name}
    return slots, parent


def run(child_name, parent_name, local_defined=(), undefined=(), vname='__ZTV10Test'):
    slots, parent = mk(child_name, parent_name)
    k = FakeKext(slots, local_defined, undefined)
    return simulate.patch_vtable(k, vname, parent)


def main():
    fails = 0

    def check(desc, result, expect_ok):
        nonlocal fails
        ok, msgs = result
        status = 'PASS' if ok == expect_ok else 'FAIL'
        if ok != expect_ok:
            fails += 1
        print('%-4s %s -> %s' % (status, desc, 'OK' if ok else 'FAIL: ' + '; '.join(msgs)))

    # 1. identical names -> skip (case 3)
    check('same name',
          run('__ZN17IO80211Controller15createWorkQueueEv',
              '__ZN17IO80211Controller15createWorkQueueEv'), True)

    # 2. parent is a pad slot and names differ -> FAIL (case 4)
    check('parent padslot, different name',
          run('__ZN17IO80211Controller22attachVirtualInterfaceEPP23IO80211VirtualInterfaceP10ether_addrjb',
              '__ZN17IO80211Controller27_RESERVEDIO80211Controller0Ev'), False)

    # 3. pure virtual child -> skip (case 2)
    check('pure virtual child',
          run('___cxa_pure_virtual', '__ZN17IO80211Controller15createWorkQueueEv'), True)

    # 4. child declared but not implemented -> FAIL (case 5)
    #    The vtable name must be the child's metaclass vtable so that the
    #    class prefix (__ZN10AirportQCA) is derived correctly.
    check('unimplemented own-class method',
          run('__ZN10AirportQCA13enableFeatureE18IO80211FeatureCodePv',
              '__ZN17IO80211Controller18someOtherMethodEv',
              undefined=['__ZN10AirportQCA13enableFeatureE18IO80211FeatureCodePv'],
              vname='__ZTVN10AirportQCA9MetaClassE'), False)

    # 5. locally defined child -> skip (case 1)
    check('locally defined child',
          run('__ZN10AirportQCA13enableFeatureE18IO80211FeatureCodePv',
              '__ZN17IO80211Controller18someOtherMethodEv',
              local_defined=['__ZN10AirportQCA13enableFeatureE18IO80211FeatureCodePv']), True)

    # 6. ordinary rebind -> patch, no failure (case 6)
    check('ordinary rebind',
          run('__ZN17IO80211Controller18someOtherMethodEv',
              '__ZN17IO80211Controller15createWorkQueueEv'), True)

    print()
    print('RESULT: %s' % ('UNIT TESTS PASS' if fails == 0 else '%d FAILURES' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
