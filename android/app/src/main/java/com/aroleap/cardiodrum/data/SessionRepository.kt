package com.aroleap.cardiodrum.data

import android.content.Context
import kotlinx.coroutines.flow.Flow

/** Thin wrapper over the DAO so the ViewModel never touches Room directly. */
class SessionRepository(context: Context) {
    private val dao = AppDatabase.get(context).sessionDao()

    val sessions: Flow<List<SessionEntity>> = dao.observeAll()

    suspend fun save(session: SessionEntity): Long = dao.insert(session)
    suspend fun delete(session: SessionEntity) = dao.delete(session)
    suspend fun clear() = dao.clear()
    suspend fun maxWork(): Double = dao.maxWork() ?: 0.0
}
