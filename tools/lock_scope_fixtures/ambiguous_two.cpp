// FIXTURE (never compiled — see README.md). The other definition of TouchStore()
// — in-memory only, so resolving the call to THIS one would hide a real hazard
// and resolving it to the other would invent one. The scan reports neither.
namespace fixture {

void TouchStore()
{
    // in-memory only
}

} // namespace fixture
