<script lang="ts">
  // The pages Thunder draws: the drop-in pages in the kit's places (chain/overrides.ts), and its own (chain/local.ts).
  import Explorer from './dropin/Explorer.svelte';
  import History from './dropin/History.svelte';
  import Home from './dropin/Home.svelte';
  import NodePage from './dropin/NodePage.svelte';
  import Overview from './dropin/Overview.svelte';
  import Peg from './dropin/Peg.svelte';
  import Receive from './dropin/Receive.svelte';
  import Send from './dropin/Send.svelte';
  import BatchPay from './pages/BatchPay.svelte';
  import Throughput from './pages/Throughput.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'home'}<Home />
{:else if page === 'overview'}<Overview />
{:else if page === 'send'}<Send />
{:else if page === 'receive'}<Receive />
{:else if page === 'transactions'}<History />
{:else if page === 'peg'}<Peg />
{:else if page === 'explorer'}<Explorer />
{:else if page === 'node'}<NodePage />
{:else if page === 'batchpay'}<BatchPay />
{:else if page === 'throughput'}<Throughput />
{/if}
