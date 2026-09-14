<script setup lang="ts">
import { ref } from 'vue'
import { moderationApi, type ReportReason, type ReportTargetType } from '@/api/moderation'
import { useToastStore } from '@/stores/toast'

const props = defineProps<{ targetType: ReportTargetType; targetId: number }>()

const toasts = useToastStore()

const open    = ref(false)
const sent    = ref(false)
const busy    = ref(false)
const reason  = ref<ReportReason>('spam')
const detail  = ref('')

const reasons: { value: ReportReason; label: string }[] = [
  { value: 'spam',        label: 'Spam' },
  { value: 'harassment',  label: 'Harassment' },
  { value: 'illegal',     label: 'Illegal content' },
  { value: 'sexual',      label: 'Sexual content' },
  { value: 'other',       label: 'Other' },
]

function start() {
  open.value = true
  sent.value = false
  reason.value = 'spam'
  detail.value = ''
}

async function submit() {
  if (busy.value) return
  busy.value = true
  try {
    await moderationApi.createReport({
      target_type: props.targetType,
      target_id: props.targetId,
      reason: reason.value,
      detail: detail.value.trim() || undefined,
    })
    sent.value = true
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not send report', 'error')
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="report">
    <button v-if="!open" class="quiet sm" @click="start">
      <span aria-hidden="true">🚩</span> Report
    </button>

    <div v-else class="card report-form">
      <template v-if="sent">
        <p class="muted">Thanks — a moderator will take a look.</p>
        <button class="quiet sm" @click="open = false">Close</button>
      </template>
      <template v-else>
        <p class="report-heading">
          Report this {{ targetType }}
        </p>
        <label class="visually-hidden" :for="`report-reason-${targetType}-${targetId}`">Reason</label>
        <select :id="`report-reason-${targetType}-${targetId}`" v-model="reason">
          <option v-for="r in reasons" :key="r.value" :value="r.value">{{ r.label }}</option>
        </select>
        <label class="visually-hidden" :for="`report-detail-${targetType}-${targetId}`">
          Additional details (optional)
        </label>
        <textarea
          :id="`report-detail-${targetType}-${targetId}`"
          v-model="detail"
          rows="2"
          maxlength="2000"
          placeholder="Additional details (optional)…"
        ></textarea>
        <div class="row tight" style="margin-top: var(--sp-2);">
          <button class="sm" :disabled="busy" @click="submit">
            {{ busy ? 'Sending…' : 'Send report' }}
          </button>
          <button type="button" class="quiet sm" @click="open = false">Cancel</button>
        </div>
      </template>
    </div>
  </div>
</template>

<style scoped>
.report-form { margin-top: var(--sp-2); padding: var(--sp-3); }
.report-heading { margin: 0 0 var(--sp-2); font-weight: 600; font-size: var(--step--1); }
.report-form select { margin-bottom: var(--sp-2); }
</style>
