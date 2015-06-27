/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file order_sl.cpp Code handling saving and loading of orders */

#include "../stdafx.h"
#include "../order_backup.h"
#include "../settings_type.h"
#include "../network/network.h"
#include "../network/network_func.h"

#include "saveload_internal.h"
#include "saveload_error.h"

/**
 * Converts this order from an old savegame's version;
 * it moves all bits to the new location.
 */
void BaseOrder::ConvertFromOldSavegame (const SavegameTypeVersion *stv)
{
	uint8 old_flags = this->flags;
	this->flags = 0;

	/* First handle non-stop - use value from savegame if possible, else use value from config file */
	if (_settings_client.gui.sg_new_nonstop || (stv->is_ottd_before (22) && stv->type != SGT_TTO && stv->type != SGT_TTD && _settings_client.gui.new_nonstop)) {
		/* OFB_NON_STOP */
		this->SetNonStopType((old_flags & 8) ? ONSF_NO_STOP_AT_ANY_STATION : ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
	} else {
		this->SetNonStopType((old_flags & 8) ? ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS : ONSF_STOP_EVERYWHERE);
	}

	/* Only a few types need the other savegame conversions. */
	switch (this->GetType()) {
		default: return;

		case OT_GOTO_STATION:
			this->SetStopLocation(OSL_PLATFORM_FAR_END);
			/* fall through */
		case OT_LOADING:
			if ((old_flags & 2) != 0) { // OFB_UNLOAD
				this->SetLoadType(OLFB_NO_LOAD);
			} else if ((old_flags & 4) == 0) { // !OFB_FULL_LOAD
				this->SetLoadType(OLF_LOAD_IF_POSSIBLE);
			} else {
				/* old OTTD versions stored full_load_any in config file - assume it was enabled when loading */
				this->SetLoadType ((_settings_client.gui.sg_full_load_any || stv->is_ottd_before (22)) ? OLF_FULL_LOAD_ANY : OLFB_FULL_LOAD);
			}

			if ((old_flags & 1) != 0) { // OFB_TRANSFER
				this->SetUnloadType(OUFB_TRANSFER);
			} else if ((old_flags & 2) != 0) { // OFB_UNLOAD
				this->SetUnloadType(OUFB_UNLOAD);
			} else {
				this->SetUnloadType(OUF_UNLOAD_IF_POSSIBLE);
			}

			break;

		case OT_GOTO_DEPOT:
			this->SetDepotActionType(((old_flags & 6) == 4) ? ODATFB_HALT : ODATF_SERVICE_ONLY);

			uint t = ((old_flags & 6) == 6) ? ODTFB_SERVICE : ODTF_MANUAL;
			if ((old_flags & 2) != 0) t |= ODTFB_PART_OF_ORDERS;
			this->SetDepotOrderType((OrderDepotTypeFlags)t);

			break;
	}
}

/**
 * Unpacks a order from savegames with version 4 and lower
 * @param packed packed order
 * @return unpacked order
 */
static BaseOrder UnpackVersion4Order (uint16 packed)
{
	return BaseOrder (GB(packed, 8, 8) << 16 | GB(packed, 4, 4) << 8 | GB(packed, 0, 4));
}

/**
 * Unpacks a order from savegames made with TTD(Patch)
 * @param packed packed order
 * @return unpacked order
 */
BaseOrder UnpackOldOrder (uint16 packed)
{
	BaseOrder order = UnpackVersion4Order (packed);

	/*
	 * Sanity check
	 * TTD stores invalid orders as OT_NOTHING with non-zero flags/station
	 */
	if (order.IsType(OT_NOTHING) && packed != 0) order.MakeDummy();

	return order;
}

const SaveLoad *GetOrderDescription()
{
	static const SaveLoad _order_desc[] = {
		 SLE_VAR(Order, type,             SLE_UINT8),
		 SLE_VAR(Order, flags,            SLE_UINT8),
		 SLE_VAR(Order, dest,             SLE_UINT16),
		 SLE_REF(Order, next,             REF_ORDER),
		 SLE_VAR(Order, refit_cargo_mask, SLE_FILE_U8 | SLE_VAR_U32,   0, 22,  36,   ),
		 SLE_VAR(Order, refit_cargo_mask, SLE_UINT32, 23,   ),
		SLE_NULL(1,                                     ,   ,  36, 181), // refit_subtype
		 SLE_VAR(Order, wait_time,        SLE_UINT16,  0,   ,  67,   ),
		 SLE_VAR(Order, travel_time,      SLE_UINT16,  0,   ,  67,   ),
		 SLE_VAR(Order, max_speed,        SLE_UINT16,  0,   , 172,   ),

		/* Leftover from the minor savegame version stuff
		 * We will never use those free bytes, but we have to keep this line to allow loading of old savegames */
		SLE_NULL(10,                                 , ,   5,  35),
		 SLE_END()
	};

	return _order_desc;
}

static void Save_ORDR(SaveDumper *dumper)
{
	Order *order;

	FOR_ALL_ORDERS(order) {
		dumper->WriteElement(order->index, order, GetOrderDescription());
	}
}

static void Load_ORDR(LoadBuffer *reader)
{
	if (reader->IsOTTDVersionBefore(5, 2)) {
		/* Legacy versions older than 5.2 did not have a ->next pointer. Convert them
		 * (in the old days, the orderlist was 5000 items big) */
		size_t len = reader->GetChunkSize();

		if (reader->IsOTTDVersionBefore(5)) {
			/* Pre-version 5 had another layout for orders
			 * (uint16 instead of uint32) */
			len /= sizeof(uint16);
			uint16 *orders = xmalloct<uint16>(len + 1);

			reader->ReadArray(orders, len, SLE_UINT16);

			for (size_t i = 0; i < len; ++i) {
				Order *o = new (i) Order();
				o->AssignOrder(UnpackVersion4Order(orders[i]));
			}

			free(orders);
		} else {
			len /= sizeof(uint32);
			uint32 *orders = xmalloct<uint32>(len + 1);

			reader->ReadArray(orders, len, SLE_UINT32);

			for (size_t i = 0; i < len; ++i) {
				new (i) Order(orders[i]);
			}

			free(orders);
		}

		/* Update all the next pointer */
		Order *o;
		FOR_ALL_ORDERS(o) {
			/* Delete invalid orders */
			if (o->IsType(OT_NOTHING)) {
				delete o;
				continue;
			}
			/* The orders were built like this:
			 * While the order is valid, set the previous will get its next pointer set */
			Order *prev = Order::GetIfValid(order_index - 1);
			if (prev != NULL) prev->next = o;
		}
	} else {
		int index;

		while ((index = reader->IterateChunk()) != -1) {
			Order *order = new (index) Order();
			reader->ReadObject(order, GetOrderDescription());
			if (reader->IsVersionBefore (19, 190)) {
				order->SetTravelTimetabled(order->GetTravelTime() > 0);
				order->SetWaitTimetabled(order->GetWaitTime() > 0);
			}
		}
	}
}

static void Ptrs_ORDR(const SavegameTypeVersion *stv)
{
	/* Orders from old savegames have pointers corrected in Load_ORDR */
	if ((stv != NULL) && stv->is_ottd_before (5, 2)) return;

	Order *o;

	FOR_ALL_ORDERS(o) {
		SlObjectPtrs(o, GetOrderDescription(), stv);
	}
}

const SaveLoad *GetOrderListDescription()
{
	static const SaveLoad _orderlist_desc[] = {
		SLE_REF(OrderList, first,              REF_ORDER),
		SLE_END()
	};

	return _orderlist_desc;
}

static void Save_ORDL(SaveDumper *dumper)
{
	OrderList *list;

	FOR_ALL_ORDER_LISTS(list) {
		dumper->WriteElement(list->index, list, GetOrderListDescription());
	}
}

static void Load_ORDL(LoadBuffer *reader)
{
	int index;

	while ((index = reader->IterateChunk()) != -1) {
		/* set num_orders to 0 so it's a valid OrderList */
		OrderList *list = new (index) OrderList(0);
		reader->ReadObject(list, GetOrderListDescription());
	}

}

static void Ptrs_ORDL(const SavegameTypeVersion *stv)
{
	OrderList *list;

	FOR_ALL_ORDER_LISTS(list) {
		SlObjectPtrs(list, GetOrderListDescription(), stv);
	}
}

const SaveLoad *GetOrderBackupDescription()
{
	/* Note that this chunk will never be loaded in a different version
	 * that it was saved (see Load_BKOR). */
	static const SaveLoad _order_backup_desc[] = {
		SLE_VAR(OrderBackup, user,                     SLE_UINT32),
		SLE_VAR(OrderBackup, tile,                     SLE_UINT32),
		SLE_VAR(OrderBackup, group,                    SLE_UINT16),
		SLE_VAR(OrderBackup, service_interval,         SLE_UINT16),
		SLE_STR(OrderBackup, name,                     SLS_NONE),
		SLE_REF(OrderBackup, clone,                    REF_VEHICLE),
		SLE_VAR(OrderBackup, cur_real_order_index,     SLE_UINT8),
		SLE_VAR(OrderBackup, cur_implicit_order_index, SLE_UINT8),
		SLE_VAR(OrderBackup, current_order_time,       SLE_UINT32),
		SLE_VAR(OrderBackup, lateness_counter,         SLE_INT32),
		SLE_VAR(OrderBackup, timetable_start,          SLE_INT32),
		SLE_VAR(OrderBackup, vehicle_flags,            SLE_UINT16),
		SLE_REF(OrderBackup, orders,                   REF_ORDER),
		SLE_END()
	};

	return _order_backup_desc;
}

static void Save_BKOR(SaveDumper *dumper)
{
	/* We only save this when we're a network server
	 * as we want this information on our clients. For
	 * normal games this information isn't needed. */
	if (!_networking || !_network_server) return;

	OrderBackup *ob;
	FOR_ALL_ORDER_BACKUPS(ob) {
		dumper->WriteElement(ob->index, ob, GetOrderBackupDescription());
	}
}

void Load_BKOR(LoadBuffer *reader)
{
	/* Only load order backups in network clients, to prevent desyncs,
	 * or when replaying, to debug them.
	 * If we are loading a savegame from disk, they are not needed and
	 * it does not make much sense to load them. */
#ifndef DEBUG_DUMP_COMMANDS
	if (!_networking || _network_server) {
		reader->SkipChunk();
		return;
	}
#endif

	/* Note that this chunk will never be loaded in a different
	 * version that it was saved. */
	if (!reader->stv->is_current()) {
		throw SlCorrupt ("Invalid savegame version");
	}

	int index;
	while ((index = reader->IterateChunk()) != -1) {
		/* set num_orders to 0 so it's a valid OrderList */
		OrderBackup *ob = new (index) OrderBackup();
		reader->ReadObject(ob, GetOrderBackupDescription());
	}
}

static void Ptrs_BKOR(const SavegameTypeVersion *stv)
{
	OrderBackup *ob;
	FOR_ALL_ORDER_BACKUPS(ob) {
		SlObjectPtrs(ob, GetOrderBackupDescription(), stv);
	}
}

extern const ChunkHandler _order_chunk_handlers[] = {
	{ 'BKOR', Save_BKOR, Load_BKOR, Ptrs_BKOR, NULL, CH_ARRAY},
	{ 'ORDR', Save_ORDR, Load_ORDR, Ptrs_ORDR, NULL, CH_ARRAY},
	{ 'ORDL', Save_ORDL, Load_ORDL, Ptrs_ORDL, NULL, CH_ARRAY | CH_LAST},
};
