#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Control.h"
#include "core/util/Hlc.h"             // Stamp: when and by whom each part of the definition was written
#include "core/util/JsonSink.h"
#include "core/util/ScratchBuffer.h"   // the members, grown by doubling as they are added

#include <cstddef>
#include <cstdint>

namespace mm {

namespace osc { class Writer; class Reader; }
namespace json { struct JsonDoc; struct JsonNode; }

/// Devices that act as one, picked from Discovery and kept by their MAC, so a rename or a new address keeps the group.
/// @card DeviceGroup.png
///
/// A device group under Devices, added and removed as an effect is; every member holds it and may edit it, and the map draws it as a box around its members.
///
/// @guide
///
/// ## Members
///
/// Add a row and pick a device; a device is offered once Discovery knows its MAC. A member unheard for three of its announcements is marked missing, here and on the map.
///
/// ## Shared
///
/// The members hold the group too, and an edit on any of them reaches the others within seconds. Deleting the group on one deletes it on all; a device removed from it drops it.
///
/// A device remembers the last eight deleted groups; a member off through more than eight deletions brings its copy back, to delete again.
///
/// @technical
///
/// **Identity.** A member is its MAC, the identity every network name derives from, so a rename or a new lease keeps it; its name is kept beside it for a member Discovery has dropped. The group is its id, the creating device's MAC and the moment it was made, as an RFC 4122 version 1 UUID is.
///
/// **Replicated, leaderless.** The shared name is a last-writer-wins register and the members a last-writer-wins element set (Shapiro et al., 2011): each member carries its stamp (Hlc.h) and a removed flag, so two members added at once both stay and a removal outlives a late copy.
///
/// **Two names.** The module's own name follows the shared one when the tree has it free, so a name taken elsewhere on one device leaves the copies agreeing.
///
/// DeviceGroupSync carries the definition between devices; this module merges it.
///
/// **Repeats by Trickle** (RFC 6206). The definition goes out at a random moment in the second half of an interval unless a member was heard sending the same one, the interval doubling from 1 s to 64 s while the members agree and back to 1 s on any difference, so the traffic stays near flat as a group grows.
///
/// **What it costs.** Zero work on the render path. A member is 56 bytes, grown by doubling against the heap's reserve; a refused growth refuses the row, or the whole restored list, and says so.
class DeviceGroup : public MoonModule, public ListSource {
public:
    /// A device group, so Devices accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::DeviceGroup; }

    /// Declare the hidden id and the members.
    void defineControls() override;

    /// Give a new group its id, once.
    void setup() override;

    /// Count the members missing every ten seconds for the status, and run the Trickle timer.
    void tick1s() MM_NONBLOCKING override;

    /// A rename of this group is an edit of its shared name, stamped and shared; the rename a merge asked for is the shared name already.
    void onModuleRenamed(const char* from, const char* to) override;

    /// An id written after setup names another device's group, as a received copy's document does: the shared name setup gave it goes, so the maker's stands.
    void onControlChanged(const char* control) override;

    // --- ListSource: the members ----------------------------------------------------------------

    /// Editable, since the members are the whole point of a group.
    bool isEditableList() const override { return true; }

    /// How many members the group holds, removed ones aside.
    ListRow listRowCount() const override;

    /// One member: its name, its MAC and whether it was heard lately.
    void writeListRow(JsonSink& sink, ListRow row) const override;

    /// A member as the saved file keeps it: its MAC, its name, its stamp and whether it was removed.
    void writeListRowSaved(JsonSink& sink, ListRow row) const override;

    /// Saved rows include the removed members, which keep a late copy from adding them back.
    ListRow listSavedRowCount() const override { return count_; }

    /// A member's one editable field, the device it is, picked from Discovery.
    void writeListRowDetail(JsonSink& sink, ListRow row) const override;

    /// The devices a member is picked from, shared by every row.
    void writeListOptionSets(JsonSink& sink) const override;

    /// Add an empty member, a device picked next.
    bool addListRow(uint32_t& outId) override;

    /// Remove one member by its row id: a removal stamped and shared.
    bool deleteListRow(uint32_t id) override;

    /// Set a member's device, by its index in the offered devices or by its MAC.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override;

    /// Rebuild the members from the saved list, their stamps observed so later edits are newer; a list that does not parse or fit leaves the members as they were.
    bool restoreList(const char* json, const char* key) override;

    // --- What Discovery, the map and DeviceGroupSync read ---------------------------------------

    /// The group's id, fixed when it was made.
    const char* groupId() const MM_NONBLOCKING { return id_; }

    /// The name the members share, which the module's own name follows when the tree has it free.
    const char* groupName() const MM_NONBLOCKING { return groupName_; }

    /// This device's writer id: the low four bytes of its MAC, which tell two of a vendor's boards apart.
    static uint32_t selfWriter();

    /// Whether a received chunk, its id read, is whole and lists `mac` as a member, and its shared name: read before any module is made for it.
    static bool chunkHasMember(osc::Reader r, const uint8_t mac[6], const char*& name);

    /// Whether `text` is a group id as makeId writes it: twenty lowercase hex digits, so a short or long one names no group.
    static bool isId(const char* text);

    /// The length of a group id, twelve hex digits of the creating MAC and eight of the moment.
    static constexpr size_t kIdLen = 20;

    /// Whether the device with this MAC is a member.
    bool hasMember(const uint8_t mac[6]) const MM_NONBLOCKING;

    /// Whether this device made the group: its id starts with this device's MAC.
    bool madeHere() const;

    /// An order-independent digest of the definition: equal on two devices exactly when they hold the same one.
    uint32_t digest() const;

    /// The newest stamp in the definition, which a deletion must beat.
    Stamp newest() const;

    /// Whether Trickle says to send the definition now; cleared by the call.
    bool sendDue() { const bool due = sendDue_; sendDue_ = false; return due; }

    /// How many messages the definition takes, `per` members each.
    uint16_t chunkCount(uint16_t per) const { return static_cast<uint16_t>(count_ / per + 1); }

    /// Write chunk `chunk` of the definition, `per` members, the name and the whole definition's digest in each.
    size_t writeChunk(uint8_t* out, size_t cap, uint16_t chunk, uint16_t per) const;

    /// Merge one received chunk, its id read by the caller, whole or not at all; true when it changed the definition, which is then saved. `wantName` names a newer name the module should take.
    bool merge(osc::Reader& r, const char*& wantName);

    /// The OSC address the definition travels on.
    static constexpr const char* kAddress = "/moonlight/devicegroup";

    /// A Stamp for a local edit, from the fleet's clock in Devices.
    Stamp stampNow();

private:
    /// One member: its row id, its identity, the name it last had, and the stamp of its last addition or removal.
    struct Member {
        uint32_t id = 0;        ///< the handle the editor addresses the row by
        uint8_t  mac[6] = {};   ///< its identity, all zero until a device is picked
        bool     removed = false;   ///< removed, kept so a late copy cannot add it back
        Stamp    stamp{};       ///< when and by whom it was added or removed
        char     name[24] = {}; ///< the name Discovery last gave it
    };

    /// A new group's id: this device's MAC and the moment, the next moment when a sibling holds it.
    void makeId();
    /// The member with this row id, or null.
    Member* find(uint32_t id);
    /// The member with this MAC, removed ones included, or null.
    Member* findMac(const uint8_t mac[6]);
    /// The index of the `row`th member not removed, or count_.
    ListRow visible(ListRow row) const;
    /// Room for one more member, false when memory refuses.
    bool grow();
    /// A local edit: saved, and Trickle back to its shortest interval so the members hear it at once.
    void edited();
    /// Start a Trickle interval of `seconds`, its send moment in the second half.
    void startInterval(uint8_t seconds);
    /// The fleet's clock, in the Devices container, or null outside one.
    Hlc* clock();
    /// Count the members and the missing among them for the status.
    void refreshStatus() MM_NONBLOCKING;
    /// How many members with a device the chunk starting at `from` carries, at most `per`.
    ListRow membersFrom(ListRow from, uint16_t per) const;
    /// One member's part of a chunk: MAC, stamp, removed, name.
    static void writeMember(osc::Writer& w, const Member& m);
    /// Whether a received chunk, its id read, carries every field its tags promise, each MAC a device's and each stamp valid.
    static bool wellFormed(osc::Reader r);
    /// One saved row restored as a member, its stamp taken into the clock; false for a row holding no device, or one already restored.
    bool restoreMember(const json::JsonDoc& doc, const json::JsonNode* el, Hlc* c);
    /// The received shared name merged, the newer stamp winning; `wantName` names it when the module should take it.
    bool mergeName(osc::Reader& r, const char*& wantName);
    /// One received member merged, the newer stamp winning; true when it changed this group.
    bool mergeMember(const char* hex, const Stamp& s, bool removed, const char* name);

    ScratchBuffer<Member> members_{*this};   ///< its count() is the capacity, doubled as members are added
    ListRow  count_ = 0;     ///< members held, removed ones included
    uint32_t nextId_ = 1;    ///< the next row id
    char     id_[kIdLen + 1] = "";   ///< twelve hex digits of the creating MAC, then eight of the moment, saved hidden
    char     groupName_[24] = "";   ///< the shared name, a last-writer-wins register, saved hidden
    Stamp    nameStamp_{};   ///< when and by whom the shared name was last set
    char     nameHlc_[40] = "";   ///< the name's stamp as "hlc:writer" in hex, saved hidden
    char     status_[40] = "";    ///< "3 members, 1 missing"
    uint8_t  statusTick_ = 0;     ///< ticks toward the next status
    uint8_t  interval_ = 1, sendAt_ = 1, elapsed_ = 0, heard_ = 0;   ///< Trickle: the interval in seconds, its send moment, seconds into it, consistent copies heard
    bool     sendDue_ = false;    ///< the send moment came with no consistent copy heard
    static constexpr uint8_t kStatusEverySec = 10;   ///< how often the status counts, which a page's read of the list shows sooner
    static constexpr uint8_t kIntervalMax = 64;      ///< Trickle's longest interval, in seconds
};

} // namespace mm
