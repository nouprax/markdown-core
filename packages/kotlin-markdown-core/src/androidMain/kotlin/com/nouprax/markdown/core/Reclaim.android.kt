package com.nouprax.markdown.core

import java.lang.ref.PhantomReference
import java.lang.ref.ReferenceQueue
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap

/** The owners [reclaim] watches, enqueued once each becomes unreachable. */
private val queue = ReferenceQueue<Any>()

/** A watch of one owner, held in [watched] until its owner is enqueued. */
private class Watch(
    owner: Any,
    val release: () -> Unit,
) : PhantomReference<Any>(owner, queue)

private val watched: MutableSet<Watch> = Collections.newSetFromMap(ConcurrentHashMap())

/** The daemon thread that runs the releases of enqueued owners. */
private val reaper: Thread by lazy {
    Thread {
        while (true) {
            val watch = queue.remove() as Watch
            watched.remove(watch)
            runCatching { watch.release() }
        }
    }.apply {
        name = "markdown-core-reclaim"
        isDaemon = true
        start()
    }
}

internal actual fun reclaim(
    owner: Any,
    release: () -> Unit,
): Any {
    reaper
    watched += Watch(owner, release)
    return Unit
}
