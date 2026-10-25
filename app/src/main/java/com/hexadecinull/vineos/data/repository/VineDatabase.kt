package com.hexadecinull.vineos.data.repository

import androidx.room.Dao
import androidx.room.Database
import androidx.room.Delete
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.RoomDatabase
import androidx.room.TypeConverter
import androidx.room.TypeConverters
import androidx.room.Update
import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase
import com.hexadecinull.vineos.data.models.VMInstance
import com.hexadecinull.vineos.data.models.VMStatus
import kotlinx.coroutines.flow.Flow

class VineConverters {
    @TypeConverter
    fun fromVMStatus(status: VMStatus): String = status.name

    @TypeConverter
    fun toVMStatus(value: String): VMStatus = runCatching { VMStatus.valueOf(value) }.getOrDefault(VMStatus.STOPPED)
}

@Dao
interface VMInstanceDao {
    @Query("SELECT * FROM vm_instances ORDER BY lastUsedAt DESC")
    fun observeAll(): Flow<List<VMInstance>>

    @Query("SELECT * FROM vm_instances WHERE id = :id")
    suspend fun getById(id: String): VMInstance?

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insert(instance: VMInstance)

    @Update
    suspend fun update(instance: VMInstance)

    @Query("UPDATE vm_instances SET status = :status WHERE id = :id")
    suspend fun updateStatus(id: String, status: VMStatus)

    @Query("UPDATE vm_instances SET lastUsedAt = :timestamp WHERE id = :id")
    suspend fun updateLastUsed(id: String, timestamp: Long = System.currentTimeMillis())

    // Nothing can be running right after the app process starts, so stale states from a crash or reboot are cleared
    @Query("UPDATE vm_instances SET status = 'STOPPED' WHERE status != 'STOPPED'")
    suspend fun resetAllStatuses()

    @Delete
    suspend fun delete(instance: VMInstance)

    @Query("DELETE FROM vm_instances WHERE id = :id")
    suspend fun deleteById(id: String)

    @Query("SELECT COUNT(*) FROM vm_instances")
    suspend fun count(): Int
}

@Database(
    entities = [VMInstance::class],
    version = 2,
    exportSchema = true,
)
@TypeConverters(VineConverters::class)
abstract class VineDatabase : RoomDatabase() {
    abstract fun vmInstanceDao(): VMInstanceDao

    companion object {
        const val DATABASE_NAME = "vineos.db"

        // Per-instance display settings
        val MIGRATION_1_2 = object : Migration(1, 2) {
            override fun migrate(db: SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE vm_instances ADD COLUMN refreshHz INTEGER NOT NULL DEFAULT 60")
                db.execSQL("ALTER TABLE vm_instances ADD COLUMN resolution TEXT")
                db.execSQL("ALTER TABLE vm_instances ADD COLUMN vsync INTEGER NOT NULL DEFAULT 1")
            }
        }
    }
}
