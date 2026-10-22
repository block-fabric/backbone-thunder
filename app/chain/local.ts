// Thunder's own pages beside the drop-in pages (chain/overrides.ts puts those in the kit's places): paying many
// recipients at once, and how busy the chain is.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  id: string;
  title: string;
  icon: IconName;
  summary: string;
  section: 'wallet' | 'chain' | 'tools';
  wallet?: boolean;
  shortcut?: string;
  keywords?: string;
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  simpleTitle?: string;
  advanced?: boolean;
  simple?: boolean;
  simpleSummary?: string;
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  { id: 'batchpay', title: 'Batch pay', icon: 'batch', section: 'wallet', summary: 'Pay many recipients in one transaction: typed, pasted or read from a CSV file', wallet: true, keywords: 'batch bulk csv many recipients payroll airdrop', quick: { label: 'Batch pay', icon: 'batch' } },
  { id: 'throughput', title: 'Throughput', simpleTitle: 'How busy', simple: true, simpleSummary: 'How busy the chain is right now', icon: 'gauge', section: 'chain', summary: 'Transactions per block, how full the blocks are, fees and the mempool', keywords: 'tps capacity block fill fees mempool' },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'th.paste', label: 'Pay a list of recipients…', page: 'batchpay', tip: 'Paste recipients and amounts, one a line', keywords: 'paste batch list csv' },
] as const satisfies readonly LocalCommand[];
