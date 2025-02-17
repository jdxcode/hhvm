/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/srcdb.h"

#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/treadmill.h"

#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/relocation.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/util/trace.h"

#include <cstdarg>
#include <cstdint>
#include <string>

namespace HPHP { namespace jit {

TRACE_SET_MOD(trans)

void IncomingBranch::patch(TCA dest) {
  switch (type()) {
    case Tag::JMP:
      smashJmp(toSmash(), dest);
      break;

    case Tag::JCC:
      smashJcc(toSmash(), dest);
      break;

    case Tag::ADDR: {
      // Note that this effectively ignores a
      TCA* addr = reinterpret_cast<TCA*>(toSmash());
      assert_address_is_atomically_accessible(addr);
      *addr = dest;
      break;
    }
  }
}

TCA IncomingBranch::target() const {
  switch (type()) {
    case Tag::JMP:
      return smashableJmpTarget(toSmash());

    case Tag::JCC:
      return smashableJccTarget(toSmash());

    case Tag::ADDR:
      return *reinterpret_cast<TCA*>(toSmash());
  }
  always_assert(false);
}

TCA TransLoc::entry() const {
  if (m_mainLen)        return mainStart();
  if (coldCodeSize())   return coldCodeStart();
  if (frozenCodeSize()) return frozenCodeStart();
  always_assert(false);
}

TcaRange TransLoc::entryRange() const {
  if (auto const size = m_mainLen)        return {mainStart(), size};
  if (auto const size = coldCodeSize())   return {coldCodeStart(), size};
  if (auto const size = frozenCodeSize()) return {frozenCodeStart(), size};
  always_assert(false);
}

TCA TransLoc::mainStart()   const { return tc::offsetToAddr(m_mainOff); }
TCA TransLoc::coldStart()   const { return tc::offsetToAddr(m_coldOff); }
TCA TransLoc::frozenStart() const { return tc::offsetToAddr(m_frozenOff); }

void TransLoc::setMainStart(TCA newStart) {
  assertx(tc::isValidCodeAddress(newStart));
  m_mainOff = tc::addrToOffset(newStart);
}

void TransLoc::setColdStart(TCA newStart) {
  assertx(tc::isValidCodeAddress(newStart));
  m_coldOff = tc::addrToOffset(newStart);
}

void TransLoc::setFrozenStart(TCA newStart) {
  assertx(tc::isValidCodeAddress(newStart));
  m_frozenOff = tc::addrToOffset(newStart);
}

/*
 * The fallback translation is where to jump to if the
 * currently-translating translation's checks fail.
 *
 * The current heuristic we use for translation chaining is to assume
 * the most common cases are probably translated first, so we chain
 * new translations on the end.  This means if we have to fallback
 * from the currently-translating translation we jump to the "anchor"
 * translation (which just is a REQ_RETRANSLATE).
 */
TCA SrcRec::getFallbackTranslation() const {
  assertx(m_anchorTranslation);
  return m_anchorTranslation;
}

void SrcRec::chainFrom(IncomingBranch br) {
  assertx(br.type() == IncomingBranch::Tag::ADDR ||
          tc::isValidCodeAddress(br.toSmash()));
  TCA destAddr = getTopTranslation();
  m_incomingBranches.push_back(br);
  TRACE(1, "SrcRec(%p)::chainFrom %p -> %p (type %d); %zd incoming branches\n",
        this,
        br.toSmash(), destAddr, static_cast<int>(br.type()),
        m_incomingBranches.size());
  br.patch(destAddr);

  if (RuntimeOption::EvalEnableReusableTC) {
    tc::recordJump(br.toSmash(), this);
  }
}

void SrcRec::newTranslation(TransLoc loc,
                            GrowableVector<IncomingBranch>& tailBranches) {
  auto srLock = writelock();
  // When translation punts due to hitting limit, will generate one
  // more translation that will call the interpreter.
  assertx(m_translations.size() <=
          std::max(RuntimeOption::EvalJitMaxProfileTranslations,
                   RuntimeOption::EvalJitMaxTranslations));

  TRACE(1, "SrcRec(%p)::newTranslation @%p, ", this, loc.entry());

  m_translations.push_back(loc);
  if (!m_topTranslation.get()) {
    m_topTranslation = loc.entry();
    patchIncomingBranches(loc.entry());
  }

  /*
   * Link all the jumps from the current tail translation to this new
   * guy.
   *
   * It's (mostly) ok if someone is running in this code while we do
   * this: we hold the write lease, they'll instead jump to the anchor
   * and do REQ_RETRANSLATE and failing to get the write lease they'll
   * interp.  FIXME: Unfortunately, right now, in an unlikely race
   * another thread could create another translation with the same
   * type specialization that we just created in this case.  (If we
   * happen to release the write lease after they jump but before they
   * get into REQ_RETRANSLATE, they'll acquire it and generate a
   * translation possibly for this same situation.)
   */
  for (auto& br : m_tailFallbackJumps) {
    br.patch(loc.entry());
  }

  // This is the new tail translation, so store the fallback jump list
  // in case we translate this again.
  m_tailFallbackJumps.swap(tailBranches);
}

/*
 * Smash the fallbacks to a particular stub (not a translation). Used
 * to smash all of the fallback jumps to resumeHelperNoTranslate when
 * we stop translating.
 */
void SrcRec::smashFallbacksToStub(TCA stub) {
  assertx(stub);
  auto srLock = writelock();
  TRACE(1, "SrcRec(%p)::smashFallbacksToStub @%p, ", this, stub);
  for (auto& br : m_tailFallbackJumps) br.patch(stub);
  m_tailFallbackJumps.clear();
}

void SrcRec::patchIncomingBranches(TCA newStart) {
  TRACE(1, "%zd incoming branches to rechain\n", m_incomingBranches.size());

  for (auto &br : m_incomingBranches) {
    TRACE(1, "SrcRec(%p)::newTranslation rechaining @%p -> %p\n",
          this, br.toSmash(), newStart);
    br.patch(newStart);
  }
}

void SrcRec::removeIncomingBranch(TCA toSmash) {
  auto srLock = writelock();

  auto end = std::remove_if(
    m_incomingBranches.begin(),
    m_incomingBranches.end(),
    [toSmash] (const IncomingBranch& ib) { return ib.toSmash() == toSmash; }
  );
  assertx(end != m_incomingBranches.end());
  m_incomingBranches.setEnd(end);
}

void SrcRec::removeIncomingBranchesInRange(TCA start, TCA frontier) {
  auto srLock = writelock();

  auto end = std::remove_if(
    m_incomingBranches.begin(),
    m_incomingBranches.end(),
    [&] (const IncomingBranch& ib) {
      auto addr = ib.toSmash();
      return start <= addr && addr < frontier;
    }
  );

  m_incomingBranches.setEnd(end);
}

void SrcRec::replaceOldTranslations() {
  auto srLock = writelock();

  // Everyone needs to give up on old translations; send them to the anchor,
  // which is a REQ_RETRANSLATE.
  auto translations = std::move(m_translations);
  m_tailFallbackJumps.clear();
  m_topTranslation = nullptr;
  assertx(!RuntimeOption::RepoAuthoritative || RuntimeOption::EvalJitPGO);
  patchIncomingBranches(m_anchorTranslation);

  // Now that we've smashed all the IBs for these translations they should be
  // unreachable-- to prevent a race we treadmill here and then reclaim their
  // associated TC space
  if (RuntimeOption::EvalEnableReusableTC) {
    tc::reclaimTranslations(std::move(translations));
    return;
  }

  translations.clear();
}

} } // HPHP::jit
