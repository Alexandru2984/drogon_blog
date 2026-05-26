import { api } from './client'

export interface FlagEvalResult {
  key:     string
  enabled: boolean
}

export interface FlagsBulkResponse {
  flags: FlagEvalResult[]
}

export interface FlagSingleResponse {
  key:     string
  known:   boolean
  enabled: boolean
}

export const flagsApi = {
  list() {
    return api.get<FlagsBulkResponse>('/flags').then(r => r.data.flags)
  },
  get(key: string) {
    return api.get<FlagSingleResponse>(`/flags/${encodeURIComponent(key)}`).then(r => r.data)
  },
}
