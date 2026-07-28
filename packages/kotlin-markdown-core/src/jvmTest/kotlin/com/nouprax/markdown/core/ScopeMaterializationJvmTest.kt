package com.nouprax.markdown.core

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ScopeMaterializationJvmTest {
    @Test
    fun commitWaitsForAnInFlightMaterialization() {
        // The deterministic interleaving from the atomic-materialization
        // contract: a reader that has acquired the live session for its
        // native scopes() call is paused inside that window, and a writer's
        // commit must not mutate the native tree until the reader publishes.
        val session = MarkupSession()
        try {
            session.append("alpha\n\nbeta\n")
            val first = session.commit()
            val document = first.document

            val readerInside = CountDownLatch(1)
            val releaseReader = CountDownLatch(1)
            ScopeResolver.materializeProbe = {
                readerInside.countDown()
                releaseReader.await()
            }
            try {
                var scope: Scope? = null
                val reader =
                    thread {
                        scope = document.scope(document.content[0])
                    }
                assertTrue(readerInside.await(5, TimeUnit.SECONDS))
                // Only the paused reader's materialization takes the probe.
                ScopeResolver.materializeProbe = null

                session.replace(0, 1, "A")
                val committed = CountDownLatch(1)
                val writer =
                    thread {
                        session.commit()
                        committed.countDown()
                    }
                // The writer detaches the previous resolver first; the
                // detach must spin behind the paused reader instead of
                // letting the native commit overlap the native scopes()
                // call.
                assertFalse(committed.await(300, TimeUnit.MILLISECONDS))

                releaseReader.countDown()
                reader.join()
                writer.join()
                assertTrue(committed.await(5, TimeUnit.SECONDS))

                // The reader materialized from the still-unchanged tree and
                // answers at the retained snapshot's revision.
                assertEquals(1, scope?.start?.line)
            } finally {
                ScopeResolver.materializeProbe = null
                releaseReader.countDown()
            }
        } finally {
            session.close()
        }
    }

    @Test
    fun closeWaitsForAnInFlightMaterialization() {
        val session = MarkupSession()
        var closed = false
        try {
            session.append("gamma\n")
            val document = session.commit().document

            val readerInside = CountDownLatch(1)
            val releaseReader = CountDownLatch(1)
            ScopeResolver.materializeProbe = {
                readerInside.countDown()
                releaseReader.await()
            }
            try {
                var scope: Scope? = null
                val reader =
                    thread {
                        scope = document.scope(document.content[0])
                    }
                assertTrue(readerInside.await(5, TimeUnit.SECONDS))
                ScopeResolver.materializeProbe = null

                val freed = CountDownLatch(1)
                val closer =
                    thread {
                        session.close()
                        closed = true
                        freed.countDown()
                    }
                // close() must not free the native session under the
                // paused reader.
                assertFalse(freed.await(300, TimeUnit.MILLISECONDS))

                releaseReader.countDown()
                reader.join()
                closer.join()
                assertTrue(freed.await(5, TimeUnit.SECONDS))
                assertEquals(1, scope?.start?.line)
            } finally {
                ScopeResolver.materializeProbe = null
                releaseReader.countDown()
            }
        } finally {
            if (!closed) {
                session.close()
            }
        }
    }
}
