//! Byte order mark occurs at the start of the document.
//!
//! ## Grammar
//!
//! Byte order mark forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! byte_order_mark ::= 0xEF 0xBB 0xBF
//! ```
//!
//! ## Recommendation
//!
//! Don’t use BOMs.
//!
//! ## Tokens
//!
//! * [`ByteOrderMark`][Name::ByteOrderMark]
//!
//! ## References
//!
//! * [`micromark/lib/preprocess.js` in `micromark`](https://github.com/micromark/micromark/blob/ed23453/packages/micromark/dev/lib/preprocess.js#L54-L60)

use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes of a BOM.
const BOM: [u8; 3] = [0xEF, 0xBB, 0xBF];

/// Before BOM.
///
/// ```text
/// > | 0xEF 0xBB 0xBF
///     ^^^^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(BOM[0]) {
        tokenizer.enter(Name::ByteOrderMark);
        State::Retry(StateName::Bom(Step::Inside { size: 0 }))
    } else {
        State::Nok
    }
}

/// In BOM.
///
/// ```text
/// > | 0xEF 0xBB 0xBF
///     ^^^^ ^^^^ ^^^^
/// ```
fn inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(BOM[size]) {
        tokenizer.consume();
        if size + 1 == BOM.len() {
            tokenizer.exit(Name::ByteOrderMark);
            State::Ok
        } else {
            State::Next(StateName::Bom(Step::Inside { size: size + 1 }))
        }
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Inside { size: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Bom(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Inside { size } => inside(tokenizer, size),
    }
}
