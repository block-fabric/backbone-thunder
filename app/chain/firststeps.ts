// The three first steps of Thunder, on Home in Simple mode until dismissed.
import type { FirstStep } from '../lib/firststeps';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring ECX here', text: 'Coins come from eCash. Drivechain gives a deposit address to pay from an eCash wallet (the Backbone app: Sidechains → Thunder). They arrive with the next block of Thunder.', icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Pay fast and cheap', text: 'Thunder has large blocks: pay one address, or many at once with Batch pay.', icon: 'send', action: { label: 'Receive', page: 'receive' } },
  { title: 'Take ECX back', text: 'Withdraw sends ECX back to eCash. That takes long: eCash’s miners approve withdrawals together, in bundles.', icon: 'up', action: { label: 'Withdraw', page: 'peg', tab: 'withdraw' } },
];
