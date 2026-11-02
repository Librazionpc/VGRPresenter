#pragma once
namespace bps::presentation {
class LineWalk {
public:
    // The answer to one step: where the walk was, where it landed, and whether it
    // moved at all. A caller that sees `moved == false` must not animate.
    struct Step {
        bool moved = false;
        bool crossedPage = false;   // the move landed on a DIFFERENT page
        int page = 0;               // page index within the pick
        int offset = 0;             // visible line within that page
        int absolute = 0;           // visible line within the RUN being drawn
    };

    // Put the walk where the picture actually is. `absolute` is the run-relative
    // line the surface is showing, not a page-relative one.
    void begin(int page, int offset, int absolute)
    {
        page_ = page < 0 ? 0 : page;
        offset_ = offset < 0 ? 0 : offset;
        absolute_ = absolute;
    }

    int page() const { return page_; }
    int offset() const { return offset_; }
    int absolute() const { return absolute_; }

    void shiftRun(int shift) { absolute_ += shift; }

    Step step(int direction, int ownLines, int pageCount, int previousPageLines)
    {
        Step s;
        s.page = page_;
        s.offset = offset_;
        s.absolute = absolute_;
        if (direction == 0 || pageCount <= 0)
            return s;                                  // moved == false

        if (direction > 0) {
            if (ownLines <= 0) {
                // An empty page (no visible line to rest on): step over it, at the
                // same absolute line — it contributes nothing to the run.
                if (page_ + 1 >= pageCount)
                    return s;
                ++page_;
                offset_ = 0;
                s.crossedPage = true;
            } else if (offset_ + 1 < ownLines) {
                ++offset_;                             // another line of this page
                ++absolute_;
            } else if (offset_ + 1 == ownLines && page_ + 1 < pageCount) {
                ++page_;                               // the next page's first line,
                offset_ = 0;                           // one line further down
                ++absolute_;
                s.crossedPage = true;
            } else {
                return s;                              // the pick's last line
            }
        } else {
            if (offset_ > 0) {
                --offset_;
                --absolute_;
            } else if (page_ > 0) {
                --page_;                               // the previous page's LAST
                if (previousPageLines > 0) {           // line, one line further up
                    offset_ = previousPageLines - 1;
                    --absolute_;
                } else {
                    offset_ = 0;                       // empty page skipped
                }
                s.crossedPage = true;
            } else {
                return s;                              // the pick's first line
            }
        }

        s.moved = true;
        s.page = page_;
        s.offset = offset_;
        s.absolute = absolute_;
        return s;
    }

private:
    int page_ = 0;
    int offset_ = 0;
    int absolute_ = 0;
};
} // namespace bps::presentation