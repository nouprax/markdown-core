//! Parts: constructs that never stand alone.
//!
//! A part is tokenized only inside another construct, which calls it where
//! its own grammar allows it: attributes after a code span’s closing ticks,
//! a label inside a link’s brackets, space or tab between two tokens.
//! micromark calls these factories.

pub mod attributes;
pub mod data;
pub mod destination;
pub mod directive;
pub mod label;
pub mod non_lazy_continuation;
pub mod space_or_tab;
pub mod space_or_tab_eol;
pub mod title;
pub mod whitespace;
