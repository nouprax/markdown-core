//! The document content type.
//!
//! **Document** represents the containers, such as block quotes, list items,
//! or GFM footnotes, which structure the document and contain other sections.
//!
//! The constructs found in flow are:
//!
//! * [Block quote][crate::construct::block_quote]
//! * [List item][crate::construct::list_item]
//! * [GFM: Footnote definition][crate::construct::footnote_definition]

use crate::construct::{
    block_quote, flow, footnote_definition, frontmatter, list_item, partial_bom,
};
use crate::event::{Content, Event, Kind, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::divide_events;
use crate::tokenizer::{Container, ContainerState, Tokenizer};
use crate::util::skip;
use alloc::{boxed::Box, vec::Vec};

/// Containers, in the order a new one is tried.
const CONTAINERS: [Container; 3] = [
    Container::BlockQuote,
    Container::ListItem,
    Container::FootnoteDefinition,
];

impl Container {
    /// State that starts a new container of this kind.
    fn start(self) -> StateName {
        match self {
            Container::BlockQuote => StateName::BlockQuote(block_quote::Step::Start),
            Container::ListItem => StateName::ListItem(list_item::Step::Start),
            Container::FootnoteDefinition => {
                StateName::FootnoteDefinition(footnote_definition::Step::Start)
            }
        }
    }

    /// State that continues an open container of this kind.
    pub fn cont(self) -> StateName {
        match self {
            Container::BlockQuote => StateName::BlockQuote(block_quote::Step::ContStart),
            Container::ListItem => StateName::ListItem(list_item::Step::ContStart),
            Container::FootnoteDefinition => {
                StateName::FootnoteDefinition(footnote_definition::Step::ContStart)
            }
        }
    }

    /// Event name of a container of this kind.
    fn name(self) -> Name {
        match self {
            Container::BlockQuote => Name::BlockQuote,
            Container::ListItem => Name::ListItem,
            Container::FootnoteDefinition => Name::FootnoteDefinition,
        }
    }
}

/// Phases where we can exit containers.
#[derive(Debug, PartialEq)]
enum Phase {
    /// After parsing a line of lazy flow which resulted in something that
    /// exits containers before the line.
    ///
    /// ```markdown
    ///   | * a
    /// > | ```js
    ///          ^
    ///   | b
    ///   | ```
    /// ```
    After,
    /// When a new container replaces an existing container.
    ///
    /// ```markdown
    ///   | * a
    /// > | > b
    ///     ^
    /// ```
    Prefix,
    /// After everything.
    ///
    /// ```markdown
    /// > | * a
    ///        ^
    /// ```
    Eof,
}

/// Start of document, at an optional BOM.
///
/// ```markdown
/// > | a
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.document.child = Some(Box::new(Tokenizer::new(
        tokenizer.point.clone(),
        tokenizer.parse_state,
    )));

    tokenizer.attempt(
        State::Next(StateName::Document(Step::BeforeFrontmatter)),
        State::Next(StateName::Document(Step::BeforeFrontmatter)),
    );

    State::Redo(StateName::Bom(partial_bom::Step::Start))
}

/// At optional frontmatter.
///
/// ```markdown
/// > | ---
///     ^
///   | title: Venus
///   | ---
/// ```
fn before_frontmatter(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Document(Step::ContainerNewBefore)),
        State::Next(StateName::Document(Step::ContainerNewBefore)),
    );
    State::Redo(StateName::Frontmatter(frontmatter::Step::Start))
}

/// At optional existing containers.
//
/// ```markdown
///   | * a
/// > | > b
///     ^
/// ```
fn container_old_before(tokenizer: &mut Tokenizer) -> State {
    // If there are more existing containers, check whether the next one continues.
    if tokenizer.document.continued < tokenizer.document.stack.len() {
        let container = &tokenizer.document.stack[tokenizer.document.continued];

        let name = container.kind.cont();

        tokenizer.attempt(
            State::Next(StateName::Document(Step::ContainerOldAfter)),
            State::Next(StateName::Document(Step::ContainerNewBefore)),
        );

        State::Redo(name)
    }
    // Otherwise, check new containers.
    else {
        State::Redo(StateName::Document(Step::ContainerNewBefore))
    }
}

/// After continued existing container.
//
/// ```markdown
///   | * a
/// > |   b
///       ^
/// ```
fn container_old_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.document.continued += 1;
    State::Redo(StateName::Document(Step::ContainerOldBefore))
}

/// At new containers.
//
/// ```markdown
/// > | * a
///     ^
/// > | > b
///     ^
/// ```
fn container_new_before(tokenizer: &mut Tokenizer) -> State {
    // If we have completely continued, restore the flow’s past `interrupt`
    // status.
    if tokenizer.document.continued == tokenizer.document.stack.len() {
        let child = tokenizer.document.child.as_ref().unwrap();

        tokenizer.interrupt = child.interrupt;

        // …and if we’re in a concrete construct, new containers can’t “pierce”
        // into them.
        if child.concrete {
            return State::Redo(StateName::Document(Step::ContainersAfter));
        }
    }

    // Add a placeholder for a new container at the end of the stack, and
    // swap it with the first existing container that did not continue.
    let tail = tokenizer.document.stack.len();
    tokenizer.document.stack.push(ContainerState {
        kind: CONTAINERS[0],
        blank_initial: false,
        size: 0,
    });
    tokenizer
        .document
        .stack
        .swap(tokenizer.document.continued, tail);
    State::Redo(StateName::Document(Step::ContainerNew { index: 0 }))
}

/// At new container `index` of [`CONTAINERS`][], or a later one.
//
/// ```markdown
/// > | * a
///     ^
/// ```
fn container_new(tokenizer: &mut Tokenizer, index: usize) -> State {
    if index == CONTAINERS.len() {
        // It wasn’t a new container.
        // Swap the placeholder (in the middle) with the existing one (at the
        // end), and drop the placeholder.
        tokenizer
            .document
            .stack
            .swap_remove(tokenizer.document.continued);
        State::Redo(StateName::Document(Step::ContainersAfter))
    } else {
        let kind = CONTAINERS[index];
        tokenizer.document.stack[tokenizer.document.continued] = ContainerState {
            kind,
            blank_initial: false,
            size: 0,
        };
        tokenizer.attempt(
            State::Next(StateName::Document(Step::ContainerNewAfter)),
            State::Next(StateName::Document(Step::ContainerNew { index: index + 1 })),
        );
        State::Redo(kind.start())
    }
}

/// After new container.
///
/// ```markdown
/// > | * a
///       ^
/// > | > b
///       ^
/// ```
fn container_new_after(tokenizer: &mut Tokenizer) -> State {
    // It was a new block quote, list item, or footnote definition.
    // Swap the new container (in the middle) with the existing one (at the end).
    // Take the new container.
    let container = tokenizer
        .document
        .stack
        .swap_remove(tokenizer.document.continued);

    // If we did not continue all existing containers, and there is a new one,
    // close the flow and those containers.
    if tokenizer.document.continued != tokenizer.document.stack.len() {
        exit_containers(tokenizer, &Phase::Prefix);
    }

    // We are “piercing” into the flow with a new container.
    tokenizer.document.child.as_mut().unwrap().pierce = true;

    tokenizer.document.stack.push(container);
    tokenizer.document.continued += 1;
    tokenizer.interrupt = false;
    State::Redo(StateName::Document(Step::ContainerNewBefore))
}

/// After containers, at flow.
//
/// ```markdown
/// > | * a
///       ^
/// > | > b
///       ^
/// ```
fn containers_after(tokenizer: &mut Tokenizer) -> State {
    let child = tokenizer.document.child.as_mut().unwrap();

    child.lax = tokenizer.document.continued != tokenizer.document.stack.len();
    // A lax line after flow that takes one (a paragraph, a callout header)
    // is read as if it interrupts that flow.
    if child.lax && tokenizer.document.open {
        child.interrupt = true;
    }
    child.define_skip(tokenizer.point.clone());

    if tokenizer.current.is_none() {
        State::Redo(StateName::Document(Step::FlowEnd))
    } else {
        let current = tokenizer.events.len();
        let previous = tokenizer.document.data;
        if let Some(previous) = previous {
            tokenizer.events[previous].link.as_mut().unwrap().next = Some(current);
        }
        tokenizer.document.data = Some(current);
        tokenizer.enter_link(
            Name::Data,
            Link {
                previous,
                next: None,
                content: Content::Flow,
            },
        );
        State::Redo(StateName::Document(Step::FlowInside))
    }
}

/// In flow.
//
/// ```markdown
/// > | * ab
///       ^
/// ```
fn flow_inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => {
            tokenizer.exit(Name::Data);
            State::Redo(StateName::Document(Step::FlowEnd))
        }
        // Note: EOL is part of data.
        Some(b'\n') => {
            tokenizer.consume();
            tokenizer.exit(Name::Data);
            State::Next(StateName::Document(Step::FlowEnd))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::Document(Step::FlowInside))
        }
    }
}

/// After flow (after eol or at eof).
//
/// ```markdown
///   | * a
/// > | > b
///     ^  ^
/// ```
fn flow_end(tokenizer: &mut Tokenizer) -> State {
    let child = tokenizer.document.child.as_mut().unwrap();
    let state = tokenizer
        .document
        .state
        .take()
        .unwrap_or(State::Next(StateName::Flow(flow::Step::Start)));

    tokenizer.document.exits.push(None);

    // The containers of this line, which a lookahead continues: a lax line
    // that does not continue a paragraph closes the ones that did not
    // continue.
    let count = if child.lax {
        tokenizer.document.continued
    } else {
        tokenizer.document.stack.len()
    };
    child.ahead.stack.clear();
    child
        .ahead
        .stack
        .extend_from_slice(&tokenizer.document.stack[..count]);

    let state = child.push(
        (child.point.index, child.point.vs),
        (tokenizer.point.index, tokenizer.point.vs),
        state,
    );

    tokenizer.document.state = Some(state);

    // If we’re in a lazy line, and the previous (lazy or not) line is something
    // that can be lazy, and this line is that too, allow it.
    //
    // Accept:
    //
    // ```markdown
    //   | * a
    // > | b
    //     ^
    //   | ```
    // ```
    //
    // Do not accept:
    //
    // ```markdown
    //   | * # a
    // > | b
    //     ^
    //   | ```
    // ```
    //
    // Do not accept:
    //
    // ```markdown
    //   | * a
    // > | # b
    //     ^
    //   | ```
    // ```
    let mut document_lax_continuation_current = false;
    let mut stack_index = child.stack.len();

    // Use two algo’s: one for when we’re suspended or in multiline things
    // like definitions, another for when we fed the line ending and closed.
    while !document_lax_continuation_current && stack_index > 0 {
        stack_index -= 1;
        let name = &child.stack[stack_index];
        if name == &Name::Content || name == &Name::TableHead {
            document_lax_continuation_current = true;
        }
    }

    // …another because we parse each “rest” line as a paragraph, and we passed
    // a EOL already.
    if !document_lax_continuation_current && !child.events.is_empty() {
        let before = skip::opt_back(&child.events, child.events.len() - 1, &[Name::LineEnd]);
        let name = &child.events[before].name;
        // A callout header takes a lax line as the start of its body.
        if name == &Name::Content || name == &Name::HeadingSetextUnderline || name == &Name::Callout
        {
            document_lax_continuation_current = true;
        }
    }

    // Reset “piercing”.
    child.pierce = false;

    if child.lax && tokenizer.document.open && document_lax_continuation_current {
        tokenizer.document.continued = tokenizer.document.stack.len();
    }

    if tokenizer.document.continued != tokenizer.document.stack.len() {
        // `Phase::After` doesn’t deal with flow: it only generates exits for
        // containers.
        exit_containers(tokenizer, &Phase::After);
    }

    if tokenizer.current.is_none() {
        tokenizer.document.continued = 0;
        exit_containers(tokenizer, &Phase::Eof);
        resolve(tokenizer);
        State::Ok
    } else {
        tokenizer.document.continued = 0;
        tokenizer.document.open = document_lax_continuation_current;
        // Containers would only be interrupting if we’ve continued.
        tokenizer.interrupt = false;
        State::Redo(StateName::Document(Step::ContainerOldBefore))
    }
}

/// Close containers (and flow if needed).
fn exit_containers(tokenizer: &mut Tokenizer, phase: &Phase) {
    let mut stack_close = tokenizer
        .document
        .stack
        .split_off(tokenizer.document.continued);

    let child = tokenizer.document.child.as_mut().unwrap();

    // Flush if needed.
    if *phase != Phase::After {
        let state = tokenizer
            .document
            .state
            .take()
            .unwrap_or(State::Next(StateName::Flow(flow::Step::Start)));

        child.flush(state, false);
    }

    if !stack_close.is_empty() {
        let index = tokenizer.document.exits.len() - (if *phase == Phase::After { 2 } else { 1 });
        let mut exits = Vec::with_capacity(stack_close.len());

        while let Some(container) = stack_close.pop() {
            let name = container.kind.name();

            exits.push(Event {
                kind: Kind::Exit,
                name,
                point: tokenizer.point.clone(),
                link: None,
            });

            let mut stack_index = tokenizer.stack.len();
            let mut found = false;

            while stack_index > 0 {
                stack_index -= 1;

                if tokenizer.stack[stack_index] == name {
                    tokenizer.stack.remove(stack_index);
                    found = true;
                    break;
                }
            }

            debug_assert!(found, "expected to find container event to exit");
        }

        debug_assert!(
            tokenizer.document.exits[index].is_none(),
            "expected no exits yet"
        );
        tokenizer.document.exits[index] = Some(exits);
    }

    child.interrupt = false;
}

// Inject everything together.
pub fn resolve(tokenizer: &mut Tokenizer) {
    let child = tokenizer.document.child.as_mut().unwrap();

    // First, add the container exits into `child`.
    let mut child_index = 0;
    let mut line = 0;

    while child_index < child.events.len() {
        if child.events[child_index].kind == Kind::Exit
            && matches!(
                child.events[child_index].name,
                Name::LineEnd | Name::BlankLineEnd
            )
        {
            // Inject before `Enter:LineEnd`.
            let mut insert_index = child_index - 1;
            let mut point = &child.events[insert_index].point;

            while child_index + 1 < child.events.len()
                && child.events[child_index + 1].kind == Kind::Exit
            {
                child_index += 1;
                point = &child.events[child_index].point;
                // Inject after `Exit:*`.
                insert_index = child_index + 1;
            }

            if line < tokenizer.document.exits.len() {
                if let Some(mut exits) = tokenizer.document.exits[line].take() {
                    let mut exit_index = 0;
                    while exit_index < exits.len() {
                        exits[exit_index].point = point.clone();
                        exit_index += 1;
                    }

                    child.map.add(insert_index, 0, exits);
                }
            }

            line += 1;
        }

        child_index += 1;
    }

    child.map.consume(&mut child.events);

    let mut flow_index = skip::to(&tokenizer.events, 0, &[Name::Data]);
    while flow_index < tokenizer.events.len()
        // To do: use `!is_some_and()` when that’s stable.
        && (tokenizer.events[flow_index].link.is_none()
            || tokenizer.events[flow_index].link.as_ref().unwrap().content != Content::Flow)
    {
        flow_index = skip::to(&tokenizer.events, flow_index + 1, &[Name::Data]);
    }

    // Now, add all child events into our parent document tokenizer.
    divide_events(
        &mut tokenizer.map,
        &tokenizer.events,
        flow_index,
        &mut child.events,
        (0, 0),
    );

    // Replace the flow data with actual events.
    tokenizer.map.consume(&mut tokenizer.events);

    // Now, add some final container exits due to the EOF.
    // We can’t inject them into the child earlier, as they are “outside” its
    // linked data.
    if line < tokenizer.document.exits.len() {
        if let Some(mut exits) = tokenizer.document.exits[line].take() {
            let mut exit_index = 0;
            while exit_index < exits.len() {
                exits[exit_index].point = tokenizer.point.clone();
                exit_index += 1;
            }

            tokenizer.events.append(&mut exits);
        }
    }

    // Add the resolvers from child.
    tokenizer
        .resolvers
        .append(&mut child.resolvers.split_off(0));

    tokenizer.definitions.append(&mut child.definitions);
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    BeforeFrontmatter,
    ContainerOldBefore,
    ContainerOldAfter,
    ContainerNewBefore,
    ContainerNew { index: usize },
    ContainerNewAfter,
    ContainersAfter,
    FlowEnd,
    FlowInside,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Document(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::BeforeFrontmatter => before_frontmatter(tokenizer),
        Step::ContainerOldBefore => container_old_before(tokenizer),
        Step::ContainerOldAfter => container_old_after(tokenizer),
        Step::ContainerNewBefore => container_new_before(tokenizer),
        Step::ContainerNew { index } => container_new(tokenizer, index),
        Step::ContainerNewAfter => container_new_after(tokenizer),
        Step::ContainersAfter => containers_after(tokenizer),
        Step::FlowEnd => flow_end(tokenizer),
        Step::FlowInside => flow_inside(tokenizer),
    }
}
