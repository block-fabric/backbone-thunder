<script lang="ts">
  // Thunder's card on Home: how busy the chain is (transactions in the latest blocks), opening Throughput.
  import Sparkline from '../components/Sparkline.svelte';
  import Card from '../views/home/Card.svelte';
  import { formatCount } from '../lib/format';
  import { t } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import { request } from '../lib/bridge';
  import type { BlockSummary } from './dropin/api';
  import { drop } from './dropin/store.svelte';

  let blocks: BlockSummary[] = $state([]);
  $effect(() => {
    void drop.version;
    request<{ blocks: BlockSummary[] }>('host', 'thunder.throughput', { count: 24 })
      .then((d) => (blocks = [...d.blocks].reverse()))
      .catch(() => undefined);
  });
  const total = $derived(blocks.reduce((s, b) => s + b.txs, 0));
</script>

<Card title={t('Throughput')} icon="gauge" open={() => go('throughput' as never)} openLabel={t('Open Throughput')}>
  <div class="big num">{formatCount(total)}<span class="unit">{t('transactions in %1 blocks', blocks.length)}</span></div>
  <Sparkline values={blocks.map((b) => b.txs)} width={240} height={36} />
</Card>

<style>
  .big {
    font-size: 1.65rem;
    font-weight: 650;
    margin-bottom: 0.4rem;
  }
  .big .unit {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
    margin-inline-start: 0.4rem;
  }
</style>
