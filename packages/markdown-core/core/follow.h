#ifndef MARKDOWN_CORE_FOLLOW_H
#define MARKDOWN_CORE_FOLLOW_H

/* THE TIMES A FOLLOWER IS ASKED (element.h, markdown_core_follow). */
typedef enum markdown_core_follow_at {
    /* The run stands where a token that made a node a later token may follow
     * ended (`lead.inline_state`); `follow` reads on from there and moves the
     * run past what it reads. */
    MARKDOWN_CORE_FOLLOW_TOKEN,
    /* A block's element offers the end of a line of its own, bytes `from` to
     * `to` of `line`; `follow` answers how many bytes before `to` it took, 0
     * for none, and `live` says the line is the one being read, whose fields
     * the line's end leaves open it reports to the parse's context sink. */
    MARKDOWN_CORE_FOLLOW_TAIL,
    /* A line after the block in its container (not asked yet: followers.md
     * F3); a follower that changes kinds there says how an edit undoes it. */
    MARKDOWN_CORE_FOLLOW_LINE,
    /* The block closes: the engine asks after the block's own element. */
    MARKDOWN_CORE_FOLLOW_CLOSE,
    /* Completion's walk completes the block, after its kind's `complete`. */
    MARKDOWN_CORE_FOLLOW_COMPLETE,
    MARKDOWN_CORE_FOLLOW_TIMES
} markdown_core_follow_at;

#endif
