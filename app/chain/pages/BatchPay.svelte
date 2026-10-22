<script lang="ts">
  // Batch pay: many recipients in one Thunder transaction (create_transfer_many): typed or pasted one a line
  // ("address, amount"), or read from a CSV file; each line checked, the total and the fee shown, then asked in the
  // app's trusted dialog with every recipient.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { base64ToBytes, openFile } from '../../lib/files';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import { parseAmount } from '../../lib/units';
  import { DROPIN } from '../dropin.config';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { confirmed, dropin, sats, satsNumber } from '../dropin/api';
  import { drop, follow, refresh } from '../dropin/store.svelte';

  $effect(() => follow());

  let text = $state('');
  let fee = $state<bigint | null>(BigInt(DROPIN.defaultFee));
  let busy = $state(false);
  let error = $state('');
  type Line = { n: number; address: string; amount: bigint | null; problem: string };
  let lines = $state<Line[]>([]);
  let checked = $state('');

  function parse(src: string): Line[] {
    const out: Line[] = [];
    src.split(/\r?\n/).forEach((raw, i) => {
      const line = raw.trim();
      if (!line || line.startsWith('#')) return;
      const parts = line.split(/[,;\t ]+/).filter(Boolean);
      if (parts.length < 2) {
        out.push({ n: i + 1, address: parts[0] ?? '', amount: null, problem: t('an address and an amount are needed') });
        return;
      }
      const parsed = parseAmount(parts[1], 'CHN');
      const amount = parsed.ok ? parsed.sats : null;
      out.push({ n: i + 1, address: parts[0], amount, problem: !parsed.ok ? t(parsed.error) : amount === null || amount <= 0n ? t('not an amount') : '' });
    });
    return out;
  }

  // Each address checked by the node, as the text settles.
  $effect(() => {
    const src = text;
    const timer = setTimeout(async () => {
      const parsed = parse(src);
      const seen = new Set<string>();
      for (const l of parsed) {
        if (l.problem) continue;
        if (seen.has(l.address)) {
          l.problem = t('this address is twice in the list');
          continue;
        }
        seen.add(l.address);
        try {
          const r = await dropin.checkAddress(l.address);
          if (!r.valid) l.problem = t(r.error ?? 'not an address');
        } catch (e) {
          l.problem = errorText(e);
        }
      }
      lines = parsed;
      checked = src;
    }, 300);
    return () => clearTimeout(timer);
  });

  const good = $derived(lines.filter((l) => !l.problem && l.amount !== null));
  const bad = $derived(lines.filter((l) => l.problem));
  const sum = $derived(good.reduce((s, l) => s + (l.amount ?? 0n), 0n));
  const total = $derived(sum + (fee ?? 0n));
  const available = $derived(sats(drop.overview?.wallet.available));
  const ready = $derived(checked === text && good.length > 0 && bad.length === 0 && fee !== null && total <= available && !busy);

  async function importCsv() {
    try {
      const f = await openFile({ title: t('A CSV file of payments'), filters: [{ name: 'CSV', patterns: ['*.csv', '*.txt'] }] });
      if (f) text = new TextDecoder().decode(base64ToBytes(f.base64));
    } catch (e) {
      toast('error', t('Import'), errorText(e));
    }
  }

  async function pay() {
    if (!ready || fee === null) return;
    busy = true;
    error = '';
    try {
      const dests: Record<string, number> = {};
      for (const l of good) dests[l.address] = satsNumber(l.amount ?? 0n);
      const txid = await confirmed<string>('create_transfer_many', [dests, satsNumber(fee)]);
      toast('success', t('Paid %1 recipients', good.length), txid);
      text = '';
      await refresh();
      go('transactions');
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page batch">
  <header class="page-head"><h1>{t('Batch pay')}</h1></header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <div class="layout">
      <section class="panel">
        <div class="panel-head row">
          <h2 class="grow">{t('Recipients')}</h2>
          <button type="button" class="btn small" onclick={importCsv}><Icon name="upload" size={15} /> {t('Import CSV…')}</button>
        </div>
        <div class="panel-body stack">
          <textarea class="mono" rows="12" spellcheck="false" bind:value={text} placeholder={`${DROPIN.addressExample}, 0.5\n${DROPIN.addressExample}, 1.25`} aria-label={t('One payment a line: address, amount')}></textarea>
          <span class="hint">{t('One payment a line: the address, then the amount in ECX. Lines starting with # are skipped.')}</span>
          {#if bad.length}
            <ul class="problems">
              {#each bad.slice(0, 8) as l (l.n)}<li><Icon name="alert" size={13} /> {t('Line %1', l.n)}: {l.problem}</li>{/each}
              {#if bad.length > 8}<li>{t('…and %1 more', bad.length - 8)}</li>{/if}
            </ul>
          {/if}
        </div>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>{t('Payment')}</h2></div>
        <div class="panel-body stack">
          <dl class="facts">
            <dt>{t('Recipients')}</dt><dd class="num">{good.length}</dd>
            <dt>{t('Amount')}</dt><dd><Amount sats={sum} /></dd>
          </dl>
          <AmountInput id="batch-fee" label={t('Fee')} bind:value={fee} required />
          <div class="summary"><span>{t('Total')}</span><strong><Amount sats={total} /></strong></div>
          <span class="hint">{t('Available')}: <Amount sats={available} /></span>
          {#if total > available}<p class="warn">{t('More than the wallet has available.')}</p>{/if}
          {#if error}<p class="error">{error}</p>{/if}
          <button type="button" class="btn primary" disabled={!ready} onclick={pay}><Icon name="send" size={16} /> {t('Review and pay %1 recipients…', good.length)}</button>
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .batch {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .layout {
    display: grid;
    grid-template-columns: minmax(0, 1.6fr) minmax(18rem, 1fr);
    gap: 1rem;
    align-items: start;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  textarea {
    width: 100%;
    resize: vertical;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.85rem;
  }
  .problems {
    list-style: none;
    margin: 0;
    padding: 0;
    color: var(--negative);
    font-size: 0.86rem;
  }
  .facts {
    display: grid;
    grid-template-columns: max-content 1fr;
    gap: 0.35rem 1rem;
    margin: 0;
  }
  .facts dt {
    color: var(--text-muted);
  }
  .facts dd {
    margin: 0;
    text-align: end;
  }
  .summary {
    display: flex;
    justify-content: space-between;
    padding: 0.6rem 0.8rem;
    border-radius: 0.6rem;
    background: color-mix(in srgb, var(--accent) 7%, transparent);
  }
  .warn {
    color: var(--warning);
    margin: 0;
  }
  .error {
    color: var(--negative);
    margin: 0;
  }
  @media (max-width: 980px) {
    .layout {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
