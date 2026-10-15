#pragma once

namespace mm {

/// The one-active-instance election, as a member a module declares and claims.
///
/// Several instances of a type may exist, two microphones say, but exactly one is the one a consumer reaches.
/// The four moves that used to be hand-copied all live here: claim, vacate if mine, vacate on destruction, and a survivor reclaiming an emptied seat.
///
/// @technical
///
/// ## The claim is idempotent, so a reclaim is just another claim
///
/// The first live instance wins and a later claimant does not displace it.
/// That one semantic serves both roles.
/// A module claims in its build hook to take the seat, and calls the same claim in its tick, where it does nothing while the seat is held.
/// The moment the holder vacates, that tick reclaims it, so there is no separate method for it.
///
/// ## Vacating on destruction is the dangling-pointer guard
///
/// The destructor vacates when this instance holds the seat, so a destroyed module can never leave the accessor pointing at freed memory.
/// That is the bug this primitive exists to make unrepresentable.
///
/// A member destructs before its module's base, so the destructor reads only this object's own seat and reference, never calling into the half-destroyed owner.
/// It is therefore safe in any order of construction, claim, vacate and destruction.
///
/// Copying and moving are deleted, the seat being tied to one instance by reference: relocating it would dangle, exactly as for the sibling scratch buffer.

template <class T>
class ActiveInstance {
public:
    /// Declared as a member of the instance it seats.
    explicit ActiveInstance(T& self) : self_(self) {}
    /// Vacates when this instance holds the seat, which is the dangling-pointer guard.
    ~ActiveInstance() { vacate(); }

    /// Non-copyable: the seat is tied to one instance by reference.
    ActiveInstance(const ActiveInstance&) = delete;
    ActiveInstance& operator=(const ActiveInstance&) = delete;
    /// Non-movable, for the same reason: relocating it would dangle.
    ActiveInstance(ActiveInstance&&) = delete;
    ActiveInstance& operator=(ActiveInstance&&) = delete;

    /// Take the seat when it is empty, the first live instance winning: @xref{the-claim-is-idempotent-so-a-reclaim-is-just-another-claim|why calling it again is safe}.
    void claim()  { if (!seat_) seat_ = &self_; }
    /// Give up the seat, but only if this instance holds it (never yanks another's seat).
    void vacate() { if (seat_ == &self_) seat_ = nullptr; }
    /// Does this instance currently hold the seat?
    bool seated() const { return seat_ == &self_; }

    /// The one live winner, or nullptr when the seat is empty. Consumers null-check.
    static T* active() { return seat_; }

private:
    T& self_;
    static inline T* seat_ = nullptr;   // one seat per T
};

} // namespace mm
