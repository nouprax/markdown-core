# Protocol transcripts

Each file of `transcripts/` is a conversation between a client and the
language engine, written as the Language Server Protocol 3.17 messages on
the wire (docs/plans/2026-10-23-lsp-completion-engine.md, section 8). The
JSON-RPC adapter `markdown-core-server` is held to them, and each platform
binding of the engine replays their requests in process and must give the
same answers, so they are the one record of what the engine answers.

A transcript is an object with a `description` and its `messages`, in order.
Each message is one of:

- `{"client": MESSAGE}`: a message the client sends, written as JSON.
- `{"raw": "TEXT"}`: a message body the client sends as these bytes, for a
  body that is not JSON or not a message.
- `{"server": MESSAGE}`: the answer the server sends to the client message
  before it. A client message that the next entry does not answer must get
  no answer.
- `{"exit": CODE}`: the server's process ends here, after `exit` or at the
  end of its input, with this exit code. No message follows it.

An answer equals its `server` message as JSON values do: objects by their
members in any order, numbers by their spelling. Error messages are part of
the answer.

The CTest `lsp_transcripts` replays every transcript through the adapter.
What the engine stores of a document's text after each change is checked
by `lsp_sync` instead, against a model of the text, since no answer of the
skeleton shows it: completion answers an empty list until the providers of
section 5 land.
