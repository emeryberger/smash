# Generated from SmashCoreConc

from __future__ import annotations
from dataclasses import dataclass
import functools
from typing import Any, Callable

@dataclass
class isFalse:
    field_0: Any

@dataclass
class isTrue:
    field_0: Any

Decidable = isFalse | isTrue

@dataclass
class PROT_RW:
    pass

@dataclass
class PROT_READ:
    pass

@dataclass
class PROT_NONE:
    pass

Prot = PROT_RW | PROT_READ | PROT_NONE

@dataclass
class EMPTY:
    pass

@dataclass
class ACTIVE:
    pass

@dataclass
class ACTIVE_MONITORING:
    pass

@dataclass
class COMPRESSING:
    pass

@dataclass
class COMPRESSED:
    pass

@dataclass
class COMPRESSED_SHADOW:
    pass

PState = EMPTY | ACTIVE | ACTIVE_MONITORING | COMPRESSING | COMPRESSED | COMPRESSED_SHADOW

@dataclass
class Page_mk:
    field_0: Any
    field_1: Any
    field_2: Any
    field_3: Any
    field_4: Any
    field_5: Any

Page = Page_mk

# Lean: SmashCoreConc.faultRestore
def fault_restore(p: Page) -> Page:
    def _f_346():
        _x_339 = ACTIVE()
        _x_340 = PROT_RW()
        _x_341 = False
        _x_342 = True
        _x_343 = p.field_4
        _x_344 = p.field_5
        _x_345 = Page_mk(_x_339, _x_340, False, True, _x_343, _x_344)
        return _x_345
    _alt_347 = _f_346
    def _f_355():
        _x_348 = ACTIVE()
        _x_349 = PROT_RW()
        _x_350 = False
        _x_351 = p.field_3
        _x_352 = p.field_4
        _x_353 = p.field_5
        _x_354 = Page_mk(_x_348, _x_349, False, _x_351, _x_352, _x_353)
        return _x_354
    _alt_356 = _f_355
    def _f_364():
        _x_357 = ACTIVE()
        _x_358 = PROT_RW()
        _x_359 = p.field_2
        _x_360 = p.field_3
        _x_361 = p.field_4
        _x_362 = p.field_5
        _x_363 = Page_mk(_x_357, _x_358, _x_359, _x_360, _x_361, _x_362)
        return _x_363
    _alt_365 = _f_364
    def _f_373():
        _x_366 = p.field_0
        _x_367 = PROT_RW()
        _x_368 = p.field_2
        _x_369 = p.field_3
        _x_370 = p.field_4
        _x_371 = p.field_5
        _x_372 = Page_mk(_x_366, _x_367, _x_368, _x_369, _x_370, _x_371)
        return _x_372
    _alt_374 = _f_373
    _alt_377 = (lambda x_375: p)
    _x_378 = p.field_0
    match _x_378:
        case COMPRESSED():
            _x_380 = _alt_347()
            return _x_380
        case COMPRESSED_SHADOW():
            _x_382 = _alt_356()
            return _x_382
        case ACTIVE_MONITORING():
            _x_384 = _alt_365()
            return _x_384
        case ACTIVE():
            _x_386 = _alt_374()
            return _x_386
        case _:
            _x_390 = (lambda h_387: _alt_377(_x_378))(None)
            return _x_390

# Lean: SmashCoreConc.appFree
def app_free(p_3: Page) -> Page:
    def _f_438(hp: Any):
        def _f_394(hq: Any):
            _x_393 = True
            return True
        _alt_395 = _f_394
        def _f_397(hq_4: Any):
            _x_396 = False
            return False
        _alt_398 = _f_397
        def _f_413(hp_5: Any):
            def _f_400(hq_6: Any):
                _x_399 = True
                return True
            _alt_401 = _f_400
            def _f_403(hq_7: Any):
                _x_402 = False
                return False
            _alt_404 = _f_403
            _x_405 = p_3.field_4
            _x_406 = False
            _x_407 = _x_405 == False
            if _x_407:
                _x_411 = _alt_401(None)
                return _x_411
            else:
                _x_409 = _alt_404(None)
                return _x_409
        _alt_414 = _f_413
        def _f_416(hp_8: Any):
            _x_415 = False
            return False
        _alt_417 = _f_416
        _x_418 = p_3.field_5
        _x_419 = False
        _x_420 = _x_418 == False
        def _jp_431(_y_425: Any):
            if _y_425:
                _x_429 = _alt_395(None)
                return _x_429
            else:
                _x_427 = _alt_398(None)
                return _x_427
        def _jp_437(_y_435: Any):
            _x_436 = _alt_414(_y_435)
            return _jp_431(_x_436)
        def _jp_434(_y_432: Any):
            _x_433 = _alt_417(_y_432)
            return _jp_431(_x_433)
        if _x_420:
            return _jp_437(None)
        else:
            return _jp_434(None)
    _alt_439 = _f_438
    def _f_441(hp_9: Any):
        _x_440 = False
        return False
    _alt_442 = _f_441
    _x_443 = p_3.field_0
    _x_444 = EMPTY()
    _x_445 = _x_443 == _x_444
    _x_446 = not _x_445
    def _jp_460(_y_451: Any):
        if _y_451:
            _x_454 = p_3.field_1
            _x_455 = False
            _x_456 = True
            _x_457 = p_3.field_5
            _x_458 = Page_mk(_x_444, _x_454, False, False, True, _x_457)
            return _x_458
        else:
            return p_3
    def _jp_463(_y_461: Any):
        _x_462 = _alt_442(_y_461)
        return _jp_460(_x_462)
    def _jp_466(_y_464: Any):
        _x_465 = _alt_439(_y_464)
        return _jp_460(_x_465)
    if _x_446:
        return _jp_466(None)
    else:
        return _jp_463(None)

# Lean: SmashCoreConc.decProcess
def dec_process(buggy: bool, p_10: Page) -> Page:
    _x_468 = p_10.field_4
    _x_469 = True
    _x_470 = _x_468 == True
    if _x_470:
        _x_473 = p_10.field_0
        _x_474 = p_10.field_1
        _x_475 = p_10.field_2
        _x_476 = p_10.field_3
        _x_477 = False
        _x_478 = Page_mk(_x_473, _x_474, _x_475, _x_476, False, True)
        _x_479 = buggy == True
        if _x_479:
            return _x_478
        else:
            _x_481 = _x_478.field_0
            _x_482 = PROT_RW()
            _x_483 = _x_478.field_2
            _x_484 = _x_478.field_3
            _x_485 = _x_478.field_4
            _x_486 = _x_478.field_5
            _x_487 = Page_mk(_x_481, _x_482, _x_483, _x_484, _x_485, _x_486)
            return _x_487
    else:
        return p_10

# Lean: SmashCoreConc.allocPop
def alloc_pop(p_11: Page) -> Page:
    def _f_506(hp_12: Any):
        def _f_493(hq_13: Any):
            _x_492 = True
            return True
        _alt_494 = _f_493
        def _f_496(hq_14: Any):
            _x_495 = False
            return False
        _alt_497 = _f_496
        _x_498 = p_11.field_0
        _x_499 = EMPTY()
        _x_500 = _x_498 == _x_499
        if _x_500:
            _x_504 = _alt_494(None)
            return _x_504
        else:
            _x_502 = _alt_497(None)
            return _x_502
    _alt_507 = _f_506
    def _f_509(hp_15: Any):
        _x_508 = False
        return False
    _alt_510 = _f_509
    _x_511 = p_11.field_5
    _x_512 = True
    _x_513 = _x_511 == True
    def _jp_528(_y_518: Any):
        if _y_518:
            _x_521 = ACTIVE()
            _x_522 = p_11.field_1
            _x_523 = p_11.field_2
            _x_524 = p_11.field_4
            _x_525 = False
            _x_526 = Page_mk(_x_521, _x_522, _x_523, True, _x_524, False)
            return _x_526
        else:
            return p_11
    def _jp_534(_y_532: Any):
        _x_533 = _alt_507(_y_532)
        return _jp_528(_x_533)
    def _jp_531(_y_529: Any):
        _x_530 = _alt_510(_y_529)
        return _jp_528(_x_530)
    if _x_513:
        return _jp_534(None)
    else:
        return _jp_531(None)

# Lean: SmashCoreConc.p2Begin
def p2begin(p_16: Page) -> Page:
    def _f_537(hp_17: Any):
        _x_536 = True
        return True
    _alt_538 = _f_537
    def _f_553(hp_18: Any):
        def _f_540(hq_19: Any):
            _x_539 = True
            return True
        _alt_541 = _f_540
        def _f_543(hq_20: Any):
            _x_542 = False
            return False
        _alt_544 = _f_543
        _x_545 = p_16.field_0
        _x_546 = ACTIVE_MONITORING()
        _x_547 = _x_545 == _x_546
        if _x_547:
            _x_551 = _alt_541(None)
            return _x_551
        else:
            _x_549 = _alt_544(None)
            return _x_549
    _alt_554 = _f_553
    _x_555 = p_16.field_0
    _x_556 = ACTIVE()
    _x_557 = _x_555 == _x_556
    def _jp_573(_y_562: Any):
        if _y_562:
            _x_565 = COMPRESSING()
            _x_566 = p_16.field_1
            _x_567 = p_16.field_2
            _x_568 = p_16.field_3
            _x_569 = p_16.field_4
            _x_570 = p_16.field_5
            _x_571 = Page_mk(_x_565, _x_566, _x_567, _x_568, _x_569, _x_570)
            return _x_571
        else:
            return p_16
    def _jp_579(_y_577: Any):
        _x_578 = _alt_538(_y_577)
        return _jp_573(_x_578)
    def _jp_576(_y_574: Any):
        _x_575 = _alt_554(_y_574)
        return _jp_573(_x_575)
    if _x_557:
        return _jp_579(None)
    else:
        return _jp_576(None)

# Lean: SmashCoreConc.p2Read
def p2read(p_21: Page) -> Page:
    _x_581 = p_21.field_0
    _x_582 = COMPRESSING()
    _x_583 = _x_581 == _x_582
    if _x_583:
        _x_586 = PROT_READ()
        _x_587 = p_21.field_2
        _x_588 = p_21.field_3
        _x_589 = p_21.field_4
        _x_590 = p_21.field_5
        _x_591 = Page_mk(_x_581, _x_586, _x_587, _x_588, _x_589, _x_590)
        return _x_591
    else:
        return p_21

# Lean: SmashCoreConc.p2Reclaim
def p2reclaim(p_22: Page) -> Page:
    _x_594 = p_22.field_0
    _x_595 = COMPRESSING()
    _x_596 = _x_594 == _x_595
    if _x_596:
        _x_599 = COMPRESSED()
        _x_600 = PROT_NONE()
        _x_601 = True
        _x_602 = False
        _x_603 = p_22.field_4
        _x_604 = p_22.field_5
        _x_605 = Page_mk(_x_599, _x_600, True, False, _x_603, _x_604)
        return _x_605
    else:
        return p_22

# Lean: SmashCoreConc.p2Defer
def p2defer(p_23: Page) -> Page:
    _x_608 = p_23.field_0
    _x_609 = COMPRESSING()
    _x_610 = _x_608 == _x_609
    if _x_610:
        _x_613 = COMPRESSED_SHADOW()
        _x_614 = PROT_RW()
        _x_615 = True
        _x_616 = p_23.field_3
        _x_617 = p_23.field_4
        _x_618 = p_23.field_5
        _x_619 = Page_mk(_x_613, _x_614, True, _x_616, _x_617, _x_618)
        return _x_619
    else:
        return p_23

# Lean: SmashCoreConc.p3CAS
def p3cas(p_24: Page) -> Page:
    _x_622 = p_24.field_0
    _x_623 = ACTIVE()
    _x_624 = _x_622 == _x_623
    if _x_624:
        _x_627 = ACTIVE_MONITORING()
        _x_628 = p_24.field_1
        _x_629 = p_24.field_2
        _x_630 = p_24.field_3
        _x_631 = p_24.field_4
        _x_632 = p_24.field_5
        _x_633 = Page_mk(_x_627, _x_628, _x_629, _x_630, _x_631, _x_632)
        return _x_633
    else:
        return p_24

# Lean: SmashCoreConc.p3Prot
def p3prot(p_25: Page) -> Page:
    _x_636 = p_25.field_0
    _x_637 = ACTIVE_MONITORING()
    _x_638 = _x_636 == _x_637
    if _x_638:
        _x_641 = PROT_READ()
        _x_642 = p_25.field_2
        _x_643 = p_25.field_3
        _x_644 = p_25.field_4
        _x_645 = p_25.field_5
        _x_646 = Page_mk(_x_636, _x_641, _x_642, _x_643, _x_644, _x_645)
        return _x_646
    else:
        return p_25

# Lean: SmashCoreConc.pbReclaim
def pb_reclaim(p_26: Page) -> Page:
    _x_649 = p_26.field_0
    _x_650 = COMPRESSED_SHADOW()
    _x_651 = _x_649 == _x_650
    if _x_651:
        _x_654 = COMPRESSED()
        _x_655 = PROT_NONE()
        _x_656 = p_26.field_2
        _x_657 = False
        _x_658 = p_26.field_4
        _x_659 = p_26.field_5
        _x_660 = Page_mk(_x_654, _x_655, _x_656, False, _x_658, _x_659)
        return _x_660
    else:
        return p_26

# Lean: SmashCoreConc.pbRestore
def pb_restore(p_27: Page) -> Page:
    _x_663 = p_27.field_0
    _x_664 = COMPRESSED_SHADOW()
    _x_665 = _x_663 == _x_664
    if _x_665:
        _x_668 = ACTIVE()
        _x_669 = PROT_RW()
        _x_670 = False
        _x_671 = p_27.field_3
        _x_672 = p_27.field_4
        _x_673 = p_27.field_5
        _x_674 = Page_mk(_x_668, _x_669, False, _x_671, _x_672, _x_673)
        return _x_674
    else:
        return p_27


