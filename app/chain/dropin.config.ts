// What the drop-in pages (chain/dropin) need to know of Thunder.
export const DROPIN = {
  /** Its slot on eCash. */
  slot: 9,
  /** The fee a payment proposes (satoshis): the node takes any; LayerTwo's app proposes about this. */
  defaultFee: 1000,
  /** The fee on eCash a deposit or a withdrawal proposes. */
  defaultMainFee: 1000,
  /** What an address looks like, for the placeholder. */
  addressExample: '4XrMw8bmnhchFspSyP4XRUuB5vaR',
};
