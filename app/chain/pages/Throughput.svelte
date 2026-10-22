<script lang="ts">
  // How busy Thunder is: transactions in the latest blocks, how full they are against the body limit of the day, the
  // fees, and what waits in the mempool (thunder.throughput).
  import Amount from '../../components/Amount.svelte';
  import Meter from '../../components/Meter.svelte';
  import Sparkline from '../../components/Sparkline.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatBytes, formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { request } from '../../lib/bridge';
  import { sats, type BlockSummary } from '../dropin/api';
  import { drop, follow } from '../dropin/store.svelte';

  $effect(() => follow());
  type Throughput = { blocks: BlockSummary[]; mempool: { txs: number; fees: number }; limit: number };
  let data = $state<Throughput | null>(null);
  let error = $state('');
  $effect(() => {
    void drop.version;
    request<Throughput>('host', 'thunder.throughput', { count: 60 })
      .then((d) => ((data = d), (error = '')))
      .catch((e) => (error = errorText(e)));
  });
  const blocks = $derived([...(data?.blocks ?? [])].reverse());
  const txs = $derived(blocks.map((b) => b.txs));
  const total = $derived(txs.reduce((a, b) => a + b, 0));
  const span = $derived(blocks.length > 1 && blocks[0].time && blocks[blocks.length - 1].time ? (blocks[blocks.length - 1].time ?? 0) - (blocks[0].time ?? 0) : 0);
  const perSecond = $derived(span > 0 ? total / span : 0);
  const last = $derived(blocks[blocks.length - 1]);
</script>

<div class="page tp">
  <header class="page-head"><h1>{t('Throughput')}</h1></header>
  {#if error}<p class="error">{error}</p>{/if}
  <div class="stats">
    <section class="panel stat">
      <span class="label">{t('Transactions, last %1 blocks', blocks.length)}</span>
      <span class="value num">{formatCount(total)}</span>
      <Sparkline values={txs} width={220} height={40} />
    </section>
    <section class="panel stat">
      <span class="label">{t('Per second')}</span>
      <span class="value num">{perSecond ? perSecond.toFixed(perSecond < 1 ? 3 : 1) : '–'}</span>
      <span class="hint">{t('Over the time eCash took to mine them.')}</span>
    </section>
    <section class="panel stat">
      <span class="label">{t('Last block')}</span>
      {#if last}
        <span class="value num">{formatCount(last.txs)} <small>{t('transactions')}</small></span>
        <Meter value={last.bytes} max={last.limit} label={t('How full')} text={`${formatBytes(last.bytes)} / ${formatBytes(last.limit)}`} />
      {:else}<span class="value">–</span>{/if}
    </section>
    <section class="panel stat">
      <span class="label">{t('Waiting in the mempool')}</span>
      <span class="value num">{formatCount(data?.mempool.txs ?? 0)}</span>
      <span class="hint">{t('Their fees')}: <Amount sats={sats(data?.mempool.fees)} /></span>
    </section>
  </div>
  <section class="panel">
    <div class="panel-head"><h2>{t('Latest blocks')}</h2></div>
    <div class="panel-body bars">
      {#each [...blocks].reverse().slice(0, 20) as b (b.hash)}
        <div class="bar">
          <span class="num h">{formatCount(b.height)}</span>
          <Meter value={b.bytes} max={b.limit} label={t('Block %1', b.height)} text={t('%1 transactions', b.txs)} tone={b.bytes > b.limit * 0.8 ? 'warning' : 'accent'} />
          <Amount sats={sats(b.fees)} />
        </div>
      {/each}
    </div>
  </section>
</div>

<style>
  .tp {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .stats {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(15rem, 1fr));
    gap: 1rem;
  }
  .stat {
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    padding: 1rem;
  }
  .label {
    color: var(--text-muted);
    font-size: 0.82rem;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .value {
    font-size: 1.6rem;
    font-weight: 650;
  }
  .value small {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.84rem;
  }
  .bars {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
  }
  .bar {
    display: grid;
    grid-template-columns: 5rem minmax(0, 1fr) 9rem;
    gap: 0.8rem;
    align-items: center;
  }
  .h {
    color: var(--text-muted);
  }
  .error {
    color: var(--negative);
  }
</style>
