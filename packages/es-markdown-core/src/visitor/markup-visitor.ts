import type { Markup } from "../markup/markup.js";

/** One required, precisely typed callback for every markup kind. */
export type MarkupVisitor = {
    [Kind in Markup["kind"]]: (this: void, node: Extract<Markup, { kind: Kind }>, phase: MarkupVisitPhase) => undefined;
};

/** The phase supplied to a markup visit. */
export type MarkupVisitPhase = "enter" | "exit";
