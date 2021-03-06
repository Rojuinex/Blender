/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2004 by Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Joseph Eagar
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/mesh/editmesh_extrude.c
 *  \ingroup edmesh
 */

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BLI_math.h"
#include "BLI_listbase.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_editmesh.h"

#include "RNA_define.h"
#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_transform.h"
#include "ED_view3d.h"

#include "UI_resources.h"

#include "MEM_guardedalloc.h"

#include "mesh_intern.h"  /* own include */

#define USE_MANIPULATOR

#ifdef USE_MANIPULATOR
#include "ED_manipulator_library.h"
#include "ED_util.h"
#endif

/* -------------------------------------------------------------------- */
/** \name Extrude Internal Utilities
 * \{ */

static void edbm_extrude_edge_exclude_mirror(
        Object *obedit, BMEditMesh *em,
        const char hflag,
        BMOperator *op, BMOpSlot *slot_edges_exclude)
{
	BMesh *bm = em->bm;
	ModifierData *md;

	/* If a mirror modifier with clipping is on, we need to adjust some
	 * of the cases above to handle edges on the line of symmetry.
	 */
	for (md = obedit->modifiers.first; md; md = md->next) {
		if ((md->type == eModifierType_Mirror) && (md->mode & eModifierMode_Realtime)) {
			MirrorModifierData *mmd = (MirrorModifierData *) md;

			if (mmd->flag & MOD_MIR_CLIPPING) {
				BMIter iter;
				BMEdge *edge;

				float mtx[4][4];
				if (mmd->mirror_ob) {
					float imtx[4][4];
					invert_m4_m4(imtx, mmd->mirror_ob->obmat);
					mul_m4_m4m4(mtx, imtx, obedit->obmat);
				}

				BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
					if (BM_elem_flag_test(edge, hflag) &&
					    BM_edge_is_boundary(edge) &&
					    BM_elem_flag_test(edge->l->f, hflag))
					{
						float co1[3], co2[3];

						copy_v3_v3(co1, edge->v1->co);
						copy_v3_v3(co2, edge->v2->co);

						if (mmd->mirror_ob) {
							mul_v3_m4v3(co1, mtx, co1);
							mul_v3_m4v3(co2, mtx, co2);
						}

						if (mmd->flag & MOD_MIR_AXIS_X) {
							if ((fabsf(co1[0]) < mmd->tolerance) &&
							    (fabsf(co2[0]) < mmd->tolerance))
							{
								BMO_slot_map_empty_insert(op, slot_edges_exclude, edge);
							}
						}
						if (mmd->flag & MOD_MIR_AXIS_Y) {
							if ((fabsf(co1[1]) < mmd->tolerance) &&
							    (fabsf(co2[1]) < mmd->tolerance))
							{
								BMO_slot_map_empty_insert(op, slot_edges_exclude, edge);
							}
						}
						if (mmd->flag & MOD_MIR_AXIS_Z) {
							if ((fabsf(co1[2]) < mmd->tolerance) &&
							    (fabsf(co2[2]) < mmd->tolerance))
							{
								BMO_slot_map_empty_insert(op, slot_edges_exclude, edge);
							}
						}
					}
				}
			}
		}
	}
}

/* individual face extrude */
/* will use vertex normals for extrusion directions, so *nor is unaffected */
static bool edbm_extrude_discrete_faces(BMEditMesh *em, wmOperator *op, const char hflag)
{
	BMOIter siter;
	BMIter liter;
	BMFace *f;
	BMLoop *l;
	BMOperator bmop;

	EDBM_op_init(
	        em, &bmop, op,
	        "extrude_discrete_faces faces=%hf use_select_history=%b",
	        hflag, true);

	/* deselect original verts */
	EDBM_flag_disable_all(em, BM_ELEM_SELECT);

	BMO_op_exec(em->bm, &bmop);

	BMO_ITER (f, &siter, bmop.slots_out, "faces.out", BM_FACE) {
		BM_face_select_set(em->bm, f, true);

		/* set face vertex normals to face normal */
		BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
			copy_v3_v3(l->v->no, f->no);
		}
	}

	if (!EDBM_op_finish(em, &bmop, op, true)) {
		return false;
	}

	return true;
}

/* extrudes individual edges */
static bool edbm_extrude_edges_indiv(BMEditMesh *em, wmOperator *op, const char hflag)
{
	BMesh *bm = em->bm;
	BMOperator bmop;

	EDBM_op_init(
	        em, &bmop, op,
	        "extrude_edge_only edges=%he use_select_history=%b",
	        hflag, true);

	/* deselect original verts */
	BM_SELECT_HISTORY_BACKUP(bm);
	EDBM_flag_disable_all(em, BM_ELEM_SELECT);
	BM_SELECT_HISTORY_RESTORE(bm);

	BMO_op_exec(em->bm, &bmop);
	BMO_slot_buffer_hflag_enable(em->bm, bmop.slots_out, "geom.out", BM_VERT | BM_EDGE, BM_ELEM_SELECT, true);

	if (!EDBM_op_finish(em, &bmop, op, true)) {
		return false;
	}

	return true;
}

/* extrudes individual vertices */
static bool edbm_extrude_verts_indiv(BMEditMesh *em, wmOperator *op, const char hflag)
{
	BMOperator bmop;

	EDBM_op_init(
	        em, &bmop, op,
	        "extrude_vert_indiv verts=%hv use_select_history=%b",
	        hflag, true);

	/* deselect original verts */
	BMO_slot_buffer_hflag_disable(em->bm, bmop.slots_in, "verts", BM_VERT, BM_ELEM_SELECT, true);

	BMO_op_exec(em->bm, &bmop);
	BMO_slot_buffer_hflag_enable(em->bm, bmop.slots_out, "verts.out", BM_VERT, BM_ELEM_SELECT, true);

	if (!EDBM_op_finish(em, &bmop, op, true)) {
		return false;
	}

	return true;
}

static char edbm_extrude_htype_from_em_select(BMEditMesh *em)
{
	char htype = BM_ALL_NOLOOP;

	if (em->selectmode & SCE_SELECT_VERTEX) {
		/* pass */
	}
	else if (em->selectmode & SCE_SELECT_EDGE) {
		htype &= ~BM_VERT;
	}
	else {
		htype &= ~(BM_VERT | BM_EDGE);
	}

	if (em->bm->totedgesel == 0) {
		htype &= ~(BM_EDGE | BM_FACE);
	}
	else if (em->bm->totfacesel == 0) {
		htype &= ~BM_FACE;
	}

	return htype;
}

static bool edbm_extrude_ex(
        Object *obedit, BMEditMesh *em,
        char htype, const char hflag,
        const bool use_mirror,
        const bool use_select_history)
{
	BMesh *bm = em->bm;
	BMOIter siter;
	BMOperator extop;
	BMElem *ele;

	/* needed to remove the faces left behind */
	if (htype & BM_FACE) {
		htype |= BM_EDGE;
	}

	BMO_op_init(bm, &extop, BMO_FLAG_DEFAULTS, "extrude_face_region");
	BMO_slot_bool_set(extop.slots_in, "use_select_history", use_select_history);
	BMO_slot_buffer_from_enabled_hflag(bm, &extop, extop.slots_in, "geom", htype, hflag);

	if (use_mirror) {
		BMOpSlot *slot_edges_exclude;
		slot_edges_exclude = BMO_slot_get(extop.slots_in, "edges_exclude");

		edbm_extrude_edge_exclude_mirror(obedit, em, hflag, &extop, slot_edges_exclude);
	}

	BM_SELECT_HISTORY_BACKUP(bm);
	EDBM_flag_disable_all(em, BM_ELEM_SELECT);
	BM_SELECT_HISTORY_RESTORE(bm);

	BMO_op_exec(bm, &extop);

	BMO_ITER (ele, &siter, extop.slots_out, "geom.out", BM_ALL_NOLOOP) {
		BM_elem_select_set(bm, ele, true);
	}

	BMO_op_finish(bm, &extop);

	return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Repeat Operator
 * \{ */

static int edbm_extrude_repeat_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	RegionView3D *rv3d = CTX_wm_region_view3d(C);

	const int steps = RNA_int_get(op->ptr, "steps");

	const float offs = RNA_float_get(op->ptr, "offset");
	float dvec[3], tmat[3][3], bmat[3][3];
	short a;

	/* dvec */
	normalize_v3_v3_length(dvec, rv3d->persinv[2], offs);

	/* base correction */
	copy_m3_m4(bmat, obedit->obmat);
	invert_m3_m3(tmat, bmat);
	mul_m3_v3(tmat, dvec);

	for (a = 0; a < steps; a++) {
		edbm_extrude_ex(obedit, em, BM_ALL_NOLOOP, BM_ELEM_SELECT, false, false);

		BMO_op_callf(
		        em->bm, BMO_FLAG_DEFAULTS,
		        "translate vec=%v verts=%hv",
		        dvec, BM_ELEM_SELECT);
	}

	EDBM_mesh_normals_update(em);

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_extrude_repeat(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Extrude Repeat Mesh";
	ot->description = "Extrude selected vertices, edges or faces repeatedly";
	ot->idname = "MESH_OT_extrude_repeat";

	/* api callbacks */
	ot->exec = edbm_extrude_repeat_exec;
	ot->poll = ED_operator_editmesh_view3d;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* props */
	RNA_def_float_distance(ot->srna, "offset", 2.0f, 0.0f, 1e12f, "Offset", "", 0.0f, 100.0f);
	RNA_def_int(ot->srna, "steps", 10, 0, 1000000, "Steps", "", 0, 180);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Operator
 * \{ */

/* generic extern called extruder */
static bool edbm_extrude_mesh(Object *obedit, BMEditMesh *em, wmOperator *op)
{
	bool changed = false;
	const char htype = edbm_extrude_htype_from_em_select(em);
	enum {NONE = 0, ELEM_FLAG, VERT_ONLY, EDGE_ONLY} nr;

	if (em->selectmode & SCE_SELECT_VERTEX) {
		if      (em->bm->totvertsel == 0) nr = NONE;
		else if (em->bm->totvertsel == 1) nr = VERT_ONLY;
		else if (em->bm->totedgesel == 0) nr = VERT_ONLY;
		else                              nr = ELEM_FLAG;
	}
	else if (em->selectmode & SCE_SELECT_EDGE) {
		if      (em->bm->totedgesel == 0) nr = NONE;
		else if (em->bm->totfacesel == 0) nr = EDGE_ONLY;
		else                              nr = ELEM_FLAG;
	}
	else {
		if      (em->bm->totfacesel == 0) nr = NONE;
		else                              nr = ELEM_FLAG;
	}

	switch (nr) {
		case NONE:
			return false;
		case ELEM_FLAG:
			changed = edbm_extrude_ex(obedit, em, htype, BM_ELEM_SELECT, true, true);
			break;
		case VERT_ONLY:
			changed = edbm_extrude_verts_indiv(em, op, BM_ELEM_SELECT);
			break;
		case EDGE_ONLY:
			changed = edbm_extrude_edges_indiv(em, op, BM_ELEM_SELECT);
			break;
	}

	if (changed) {
		return true;
	}
	else {
		BKE_report(op->reports, RPT_ERROR, "Not a valid selection for extrude");
		return false;
	}
}

/* extrude without transform */
static int edbm_extrude_region_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	edbm_extrude_mesh(obedit, em, op);

	/* This normally happens when pushing undo but modal operators
	 * like this one don't push undo data until after modal mode is
	 * done.*/
	EDBM_mesh_normals_update(em);

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_extrude_region(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Extrude Region";
	ot->idname = "MESH_OT_extrude_region";
	ot->description = "Extrude region of faces";

	/* api callbacks */
	//ot->invoke = mesh_extrude_region_invoke;
	ot->exec = edbm_extrude_region_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	Transform_Properties(ot, P_NO_DEFAULTS | P_MIRROR_DUMMY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Verts Operator
 * \{ */

static int edbm_extrude_verts_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	edbm_extrude_verts_indiv(em, op, BM_ELEM_SELECT);

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_extrude_verts_indiv(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Extrude Only Vertices";
	ot->idname = "MESH_OT_extrude_verts_indiv";
	ot->description = "Extrude individual vertices only";

	/* api callbacks */
	ot->exec = edbm_extrude_verts_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* to give to transform */
	Transform_Properties(ot, P_NO_DEFAULTS | P_MIRROR_DUMMY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Edges Operator
 * \{ */

static int edbm_extrude_edges_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	edbm_extrude_edges_indiv(em, op, BM_ELEM_SELECT);

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_extrude_edges_indiv(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Extrude Only Edges";
	ot->idname = "MESH_OT_extrude_edges_indiv";
	ot->description = "Extrude individual edges only";

	/* api callbacks */
	ot->exec = edbm_extrude_edges_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* to give to transform */
	Transform_Properties(ot, P_NO_DEFAULTS | P_MIRROR_DUMMY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Faces Operator
 * \{ */

static int edbm_extrude_faces_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	edbm_extrude_discrete_faces(em, op, BM_ELEM_SELECT);

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_extrude_faces_indiv(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Extrude Individual Faces";
	ot->idname = "MESH_OT_extrude_faces_indiv";
	ot->description = "Extrude individual faces only";

	/* api callbacks */
	ot->exec = edbm_extrude_faces_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	Transform_Properties(ot, P_NO_DEFAULTS | P_MIRROR_DUMMY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dupli-Extrude Operator
 *
 * Add-click-mesh (extrude) operator.
 * \{ */

static int edbm_dupli_extrude_cursor_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	ViewContext vc;
	BMVert *v1;
	BMIter iter;
	float center[3];
	uint verts_len;
	bool use_proj;

	em_setup_viewcontext(C, &vc);

	invert_m4_m4(vc.obedit->imat, vc.obedit->obmat);

	ED_view3d_init_mats_rv3d(vc.obedit, vc.rv3d);

	use_proj = ((vc.scene->toolsettings->snap_flag & SCE_SNAP) &&
	            (vc.scene->toolsettings->snap_mode == SCE_SNAP_MODE_FACE));

	zero_v3(center);
	verts_len = 0;

	BM_ITER_MESH (v1, &iter, vc.em->bm, BM_VERTS_OF_MESH) {
		if (BM_elem_flag_test(v1, BM_ELEM_SELECT)) {
			add_v3_v3(center, v1->co);
			verts_len += 1;
		}
	}

	/* call extrude? */
	if (verts_len != 0) {
		const char extrude_htype = edbm_extrude_htype_from_em_select(vc.em);
		const bool rot_src = RNA_boolean_get(op->ptr, "rotate_source");
		BMEdge *eed;
		float mat[3][3];
		float vec[3], ofs[3];
		float nor[3] = {0.0, 0.0, 0.0};

		/* 2D normal calc */
		const float mval_f[2] = {(float)event->mval[0],
		                         (float)event->mval[1]};

		mul_v3_fl(center, 1.0f / (float)verts_len);

		/* check for edges that are half selected, use for rotation */
		bool done = false;
		BM_ITER_MESH (eed, &iter, vc.em->bm, BM_EDGES_OF_MESH) {
			if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
				float co1[2], co2[2];

				if ((ED_view3d_project_float_object(vc.ar, eed->v1->co, co1, V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK) &&
				    (ED_view3d_project_float_object(vc.ar, eed->v2->co, co2, V3D_PROJ_TEST_NOP) == V3D_PROJ_RET_OK))
				{
					/* 2D rotate by 90d while adding.
					 *  (x, y) = (y, -x)
					 *
					 * accumulate the screenspace normal in 2D,
					 * with screenspace edge length weighting the result. */
					if (line_point_side_v2(co1, co2, mval_f) >= 0.0f) {
						nor[0] +=  (co1[1] - co2[1]);
						nor[1] += -(co1[0] - co2[0]);
					}
					else {
						nor[0] +=  (co2[1] - co1[1]);
						nor[1] += -(co2[0] - co1[0]);
					}
					done = true;
				}
			}
		}

		if (done) {
			float view_vec[3], cross[3];

			/* convert the 2D nomal into 3D */
			mul_mat3_m4_v3(vc.rv3d->viewinv, nor); /* worldspace */
			mul_mat3_m4_v3(vc.obedit->imat, nor); /* local space */

			/* correct the normal to be aligned on the view plane */
			mul_v3_mat3_m4v3(view_vec, vc.obedit->imat, vc.rv3d->viewinv[2]);
			cross_v3_v3v3(cross, nor, view_vec);
			cross_v3_v3v3(nor, view_vec, cross);
			normalize_v3(nor);
		}

		/* center */
		copy_v3_v3(ofs, center);

		mul_m4_v3(vc.obedit->obmat, ofs);  /* view space */
		ED_view3d_win_to_3d_int(vc.v3d, vc.ar, ofs, event->mval, ofs);
		mul_m4_v3(vc.obedit->imat, ofs); // back in object space

		sub_v3_v3(ofs, center);

		/* calculate rotation */
		unit_m3(mat);
		if (done) {
			float angle;

			normalize_v3_v3(vec, ofs);

			angle = angle_normalized_v3v3(vec, nor);

			if (angle != 0.0f) {
				float axis[3];

				cross_v3_v3v3(axis, nor, vec);

				/* halve the rotation if its applied twice */
				if (rot_src) {
					angle *= 0.5f;
				}

				axis_angle_to_mat3(mat, axis, angle);
			}
		}

		if (rot_src) {
			EDBM_op_callf(vc.em, op, "rotate verts=%hv cent=%v matrix=%m3",
			              BM_ELEM_SELECT, center, mat);

			/* also project the source, for retopo workflow */
			if (use_proj)
				EMBM_project_snap_verts(C, vc.ar, vc.em);
		}

		edbm_extrude_ex(vc.obedit, vc.em, extrude_htype, BM_ELEM_SELECT, true, true);
		EDBM_op_callf(vc.em, op, "rotate verts=%hv cent=%v matrix=%m3",
		              BM_ELEM_SELECT, center, mat);
		EDBM_op_callf(vc.em, op, "translate verts=%hv vec=%v",
		              BM_ELEM_SELECT, ofs);
	}
	else {
		const float *cursor = ED_view3d_cursor3d_get(vc.scene, vc.v3d);
		BMOperator bmop;
		BMOIter oiter;

		copy_v3_v3(center, cursor);
		ED_view3d_win_to_3d_int(vc.v3d, vc.ar, center, event->mval, center);

		mul_m4_v3(vc.obedit->imat, center); // back in object space

		EDBM_op_init(vc.em, &bmop, op, "create_vert co=%v", center);
		BMO_op_exec(vc.em->bm, &bmop);

		BMO_ITER (v1, &oiter, bmop.slots_out, "vert.out", BM_VERT) {
			BM_vert_select_set(vc.em->bm, v1, true);
		}

		if (!EDBM_op_finish(vc.em, &bmop, op, true)) {
			return OPERATOR_CANCELLED;
		}
	}

	if (use_proj)
		EMBM_project_snap_verts(C, vc.ar, vc.em);

	/* This normally happens when pushing undo but modal operators
	 * like this one don't push undo data until after modal mode is
	 * done. */
	EDBM_mesh_normals_update(vc.em);

	EDBM_update_generic(vc.em, true, true);

	return OPERATOR_FINISHED;
}

void MESH_OT_dupli_extrude_cursor(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Duplicate or Extrude to Cursor";
	ot->idname = "MESH_OT_dupli_extrude_cursor";
	ot->description = "Duplicate and extrude selected vertices, edges or faces towards the mouse cursor";

	/* api callbacks */
	ot->invoke = edbm_dupli_extrude_cursor_invoke;
	ot->poll = ED_operator_editmesh_region_view3d;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	RNA_def_boolean(ot->srna, "rotate_source", true, "Rotate Source", "Rotate initial selection giving better shape");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spin Operator
 * \{ */

static int edbm_spin_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMesh *bm = em->bm;
	BMOperator spinop;
	float cent[3], axis[3];
	float d[3] = {0.0f, 0.0f, 0.0f};
	int steps, dupli;
	float angle;

	RNA_float_get_array(op->ptr, "center", cent);
	RNA_float_get_array(op->ptr, "axis", axis);
	steps = RNA_int_get(op->ptr, "steps");
	angle = RNA_float_get(op->ptr, "angle");
	//if (ts->editbutflag & B_CLOCKWISE)
	angle = -angle;
	dupli = RNA_boolean_get(op->ptr, "dupli");

	if (is_zero_v3(axis)) {
		BKE_report(op->reports, RPT_ERROR, "Invalid/unset axis");
		return OPERATOR_CANCELLED;
	}

	/* keep the values in worldspace since we're passing the obmat */
	if (!EDBM_op_init(em, &spinop, op,
	                  "spin geom=%hvef cent=%v axis=%v dvec=%v steps=%i angle=%f space=%m4 use_duplicate=%b",
	                  BM_ELEM_SELECT, cent, axis, d, steps, angle, obedit->obmat, dupli))
	{
		return OPERATOR_CANCELLED;
	}
	BMO_op_exec(bm, &spinop);
	EDBM_flag_disable_all(em, BM_ELEM_SELECT);
	BMO_slot_buffer_hflag_enable(bm, spinop.slots_out, "geom_last.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);
	if (!EDBM_op_finish(em, &spinop, op, true)) {
		return OPERATOR_CANCELLED;
	}

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

/* get center and axis, in global coords */
static int edbm_spin_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = CTX_wm_view3d(C);
	RegionView3D *rv3d = ED_view3d_context_rv3d(C);

	PropertyRNA *prop;
	prop = RNA_struct_find_property(op->ptr, "center");
	if (!RNA_property_is_set(op->ptr, prop)) {
		RNA_property_float_set_array(op->ptr, prop, ED_view3d_cursor3d_get(scene, v3d));
	}
	if (rv3d) {
		prop = RNA_struct_find_property(op->ptr, "axis");
		if (!RNA_property_is_set(op->ptr, prop)) {
			RNA_property_float_set_array(op->ptr, prop, rv3d->viewinv[2]);
		}
	}

	int ret = edbm_spin_exec(C, op);

#ifdef USE_MANIPULATOR
	if (ret & OPERATOR_FINISHED) {
		/* Setup manipulators */
		if (v3d && (v3d->twtype & V3D_MANIPULATOR_DRAW)) {
			WM_manipulator_group_type_ensure("MESH_WGT_spin");
		}
	}
#endif

	return ret;

}

#ifdef USE_MANIPULATOR
static void MESH_WGT_spin(struct wmManipulatorGroupType *wgt);
#endif

void MESH_OT_spin(wmOperatorType *ot)
{
	PropertyRNA *prop;

	/* identifiers */
	ot->name = "Spin";
	ot->description = "Extrude selected vertices in a circle around the cursor in indicated viewport";
	ot->idname = "MESH_OT_spin";

	/* api callbacks */
	ot->invoke = edbm_spin_invoke;
	ot->exec = edbm_spin_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* props */
	RNA_def_int(ot->srna, "steps", 9, 0, 1000000, "Steps", "Steps", 0, 1000);
	RNA_def_boolean(ot->srna, "dupli", 0, "Dupli", "Make Duplicates");
	prop = RNA_def_float(ot->srna, "angle", DEG2RADF(90.0f), -1e12f, 1e12f, "Angle", "Rotation for each step",
	                     DEG2RADF(-360.0f), DEG2RADF(360.0f));
	RNA_def_property_subtype(prop, PROP_ANGLE);

	RNA_def_float_vector(ot->srna, "center", 3, NULL, -1e12f, 1e12f,
	                     "Center", "Center in global view space", -1e4f, 1e4f);
	RNA_def_float_vector(ot->srna, "axis", 3, NULL, -1.0f, 1.0f, "Axis", "Axis in global view space", -1.0f, 1.0f);

#ifdef USE_MANIPULATOR
	WM_manipulatorgrouptype_append(MESH_WGT_spin);
#endif
}


#ifdef USE_MANIPULATOR

/* -------------------------------------------------------------------- */
/** \name Screw Operator
 * \{ */

typedef struct ManipulatorSpinGroup {
	/* Arrow to change plane depth. */
	struct wmManipulator *translate_z;
	/* Translate XYZ */
	struct wmManipulator *translate_c;
	/* For grabbing the manipulator and moving freely. */
	struct wmManipulator *rotate_c;
	/* Spin angle */
	struct wmManipulator *angle_z;

	/* We could store more vars here! */
	struct {
		bContext *context;
		wmOperator *op;
		PropertyRNA *prop_axis_co;
		PropertyRNA *prop_axis_no;
		PropertyRNA *prop_angle;

		float rotate_axis[3];
		float rotate_up[3];
	} data;
} ManipulatorSpinGroup;

/**
 * XXX. calling redo from property updates is not great.
 * This is needed because changing the RNA doesn't cause a redo
 * and we're not using operator UI which does just this.
 */
static void manipulator_spin_exec(ManipulatorSpinGroup *man)
{
	wmOperator *op = man->data.op;
	if (op == WM_operator_last_redo((bContext *)man->data.context)) {
		ED_undo_operator_repeat((bContext *)man->data.context, op);
	}
}

static void manipulator_mesh_spin_update_from_op(ManipulatorSpinGroup *man)
{
	wmOperator *op = man->data.op;

	float plane_co[3], plane_no[3];

	RNA_property_float_get_array(op->ptr, man->data.prop_axis_co, plane_co);
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_no, plane_no);

	WM_manipulator_set_matrix_location(man->translate_z, plane_co);
	WM_manipulator_set_matrix_location(man->rotate_c, plane_co);
	WM_manipulator_set_matrix_location(man->angle_z, plane_co);
	/* translate_c location comes from the property. */

	WM_manipulator_set_matrix_rotation_from_z_axis(man->translate_z, plane_no);
	WM_manipulator_set_matrix_rotation_from_z_axis(man->angle_z, plane_no);

	WM_manipulator_set_scale(man->translate_c, 0.2);

	RegionView3D *rv3d = ED_view3d_context_rv3d(man->data.context);
	if (rv3d) {
		normalize_v3_v3(man->data.rotate_axis, rv3d->viewinv[2]);
		normalize_v3_v3(man->data.rotate_up, rv3d->viewinv[1]);

		/* ensure its orthogonal */
		project_plane_normalized_v3_v3v3(man->data.rotate_up, man->data.rotate_up, man->data.rotate_axis);
		normalize_v3(man->data.rotate_up);

		WM_manipulator_set_matrix_rotation_from_z_axis(man->translate_c, plane_no);
		WM_manipulator_set_matrix_rotation_from_yz_axis(man->rotate_c, plane_no, man->data.rotate_axis);

		/* show the axis instead of mouse cursor */
		RNA_enum_set(man->rotate_c->ptr, "draw_options",
		             ED_MANIPULATOR_DIAL_DRAW_FLAG_ANGLE_MIRROR |
		             ED_MANIPULATOR_DIAL_DRAW_FLAG_ANGLE_START_Y);

	}
}

/* depth callbacks */
static void manipulator_spin_prop_depth_get(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);

	float plane_co[3], plane_no[3];
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_co, plane_co);
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_no, plane_no);

	value[0] = dot_v3v3(plane_no, plane_co) - dot_v3v3(plane_no, mpr->matrix_basis[3]);
}

static void manipulator_spin_prop_depth_set(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        const void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	const float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);

	float plane_co[3], plane[4];
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_co, plane_co);
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_no, plane);
	normalize_v3(plane);

	plane[3] = -value[0] - dot_v3v3(plane, mpr->matrix_basis[3]);

	/* Keep our location, may be offset simply to be inside the viewport. */
	closest_to_plane_normalized_v3(plane_co, plane, plane_co);

	RNA_property_float_set_array(op->ptr, man->data.prop_axis_co, plane_co);

	manipulator_spin_exec(man);
}

/* translate callbacks */
static void manipulator_spin_prop_translate_get(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 3);
	UNUSED_VARS_NDEBUG(mpr_prop);

	RNA_property_float_get_array(op->ptr, man->data.prop_axis_co, value);
}

static void manipulator_spin_prop_translate_set(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        const void *value)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;

	BLI_assert(mpr_prop->type->array_length == 3);
	UNUSED_VARS_NDEBUG(mpr_prop);

	RNA_property_float_set_array(op->ptr, man->data.prop_axis_co, value);

	manipulator_spin_exec(man);
}

/* angle callbacks */
static void manipulator_spin_prop_axis_angle_get(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);

	float plane_no[4];
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_no, plane_no);
	normalize_v3(plane_no);

	float plane_no_proj[3];
	project_plane_normalized_v3_v3v3(plane_no_proj, plane_no, man->data.rotate_axis);

	if (!is_zero_v3(plane_no_proj)) {
		const float angle = -angle_signed_on_axis_v3v3_v3(plane_no_proj, man->data.rotate_up, man->data.rotate_axis);
		value[0] = angle;
	}
	else {
		value[0] = 0.0f;
	}
}

static void manipulator_spin_prop_axis_angle_set(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        const void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	const float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);

	float plane_no[4];
	RNA_property_float_get_array(op->ptr, man->data.prop_axis_no, plane_no);
	normalize_v3(plane_no);

	float plane_no_proj[3];
	project_plane_normalized_v3_v3v3(plane_no_proj, plane_no, man->data.rotate_axis);

	if (!is_zero_v3(plane_no_proj)) {
		const float angle = -angle_signed_on_axis_v3v3_v3(plane_no_proj, man->data.rotate_up, man->data.rotate_axis);
		const float angle_delta = angle - angle_compat_rad(value[0], angle);
		if (angle_delta != 0.0f) {
			float mat[3][3];
			axis_angle_normalized_to_mat3(mat, man->data.rotate_axis, angle_delta);
			mul_m3_v3(mat, plane_no);

			/* re-normalize - seems acceptable */
			RNA_property_float_set_array(op->ptr, man->data.prop_axis_no, plane_no);

			manipulator_spin_exec(man);
		}
	}
}

/* angle callbacks */
static void manipulator_spin_prop_angle_get(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	float *value = value_p;

	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);
	value[0] = RNA_property_float_get(op->ptr, man->data.prop_angle);
}

static void manipulator_spin_prop_angle_set(
        const wmManipulator *mpr, wmManipulatorProperty *mpr_prop,
        const void *value_p)
{
	ManipulatorSpinGroup *man = mpr->parent_mgroup->customdata;
	wmOperator *op = man->data.op;
	BLI_assert(mpr_prop->type->array_length == 1);
	UNUSED_VARS_NDEBUG(mpr_prop);
	const float *value = value_p;
	RNA_property_float_set(op->ptr, man->data.prop_angle, value[0]);

	manipulator_spin_exec(man);
}

static bool manipulator_mesh_spin_poll(const bContext *C, wmManipulatorGroupType *wgt)
{
	wmOperator *op = WM_operator_last_redo(C);
	if (op == NULL || !STREQ(op->type->idname, "MESH_OT_spin")) {
		WM_manipulator_group_type_unlink_delayed_ptr(wgt);
		return false;
	}
	return true;
}

static void manipulator_mesh_spin_setup(const bContext *C, wmManipulatorGroup *mgroup)
{
	wmOperator *op = WM_operator_last_redo(C);

	if (op == NULL || !STREQ(op->type->idname, "MESH_OT_spin")) {
		return;
	}

	struct ManipulatorSpinGroup *man = MEM_callocN(sizeof(ManipulatorSpinGroup), __func__);
	mgroup->customdata = man;

	const wmManipulatorType *wt_arrow = WM_manipulatortype_find("MANIPULATOR_WT_arrow_3d", true);
	const wmManipulatorType *wt_grab = WM_manipulatortype_find("MANIPULATOR_WT_grab_3d", true);
	const wmManipulatorType *wt_dial = WM_manipulatortype_find("MANIPULATOR_WT_dial_3d", true);

	man->translate_z = WM_manipulator_new_ptr(wt_arrow, mgroup, NULL);
	man->translate_c = WM_manipulator_new_ptr(wt_grab, mgroup, NULL);
	man->rotate_c = WM_manipulator_new_ptr(wt_dial, mgroup, NULL);
	man->angle_z = WM_manipulator_new_ptr(wt_dial, mgroup, NULL);

	UI_GetThemeColor3fv(TH_MANIPULATOR_PRIMARY, man->translate_z->color);
	UI_GetThemeColor3fv(TH_MANIPULATOR_PRIMARY, man->translate_c->color);
	UI_GetThemeColor3fv(TH_MANIPULATOR_SECONDARY, man->rotate_c->color);
	UI_GetThemeColor3fv(TH_AXIS_Z, man->angle_z->color);


	RNA_enum_set(man->translate_z->ptr, "draw_style", ED_MANIPULATOR_ARROW_STYLE_NORMAL);
	RNA_enum_set(man->translate_c->ptr, "draw_style", ED_MANIPULATOR_GRAB_STYLE_RING_2D);

	WM_manipulator_set_flag(man->translate_c, WM_MANIPULATOR_DRAW_VALUE, true);
	WM_manipulator_set_flag(man->rotate_c, WM_MANIPULATOR_DRAW_VALUE, true);
	WM_manipulator_set_flag(man->angle_z, WM_MANIPULATOR_DRAW_VALUE, true);

	WM_manipulator_set_scale(man->angle_z, 0.5f);

	{
		man->data.context = (bContext *)C;
		man->data.op = op;
		man->data.prop_axis_co = RNA_struct_find_property(op->ptr, "center");
		man->data.prop_axis_no = RNA_struct_find_property(op->ptr, "axis");
		man->data.prop_angle = RNA_struct_find_property(op->ptr, "angle");
	}

	manipulator_mesh_spin_update_from_op(man);

	/* Setup property callbacks */
	{
		WM_manipulator_target_property_def_func(
		        man->translate_z, "offset",
		        &(const struct wmManipulatorPropertyFnParams) {
		            .value_get_fn = manipulator_spin_prop_depth_get,
		            .value_set_fn = manipulator_spin_prop_depth_set,
		            .range_get_fn = NULL,
		            .user_data = NULL,
		        });

		WM_manipulator_target_property_def_func(
		        man->translate_c, "offset",
		        &(const struct wmManipulatorPropertyFnParams) {
		            .value_get_fn = manipulator_spin_prop_translate_get,
		            .value_set_fn = manipulator_spin_prop_translate_set,
		            .range_get_fn = NULL,
		            .user_data = NULL,
		        });

		WM_manipulator_target_property_def_func(
		        man->rotate_c, "offset",
		        &(const struct wmManipulatorPropertyFnParams) {
		            .value_get_fn = manipulator_spin_prop_axis_angle_get,
		            .value_set_fn = manipulator_spin_prop_axis_angle_set,
		            .range_get_fn = NULL,
		            .user_data = NULL,
		        });

		WM_manipulator_target_property_def_func(
		        man->angle_z, "offset",
		        &(const struct wmManipulatorPropertyFnParams) {
		            .value_get_fn = manipulator_spin_prop_angle_get,
		            .value_set_fn = manipulator_spin_prop_angle_set,
		            .range_get_fn = NULL,
		            .user_data = NULL,
		        });

	}
}

static void manipulator_mesh_spin_draw_prepare(
        const bContext *UNUSED(C), wmManipulatorGroup *mgroup)
{
	ManipulatorSpinGroup *man = mgroup->customdata;
	if (man->data.op->next) {
		man->data.op = WM_operator_last_redo((bContext *)man->data.context);
	}
	manipulator_mesh_spin_update_from_op(man);
}

static void MESH_WGT_spin(struct wmManipulatorGroupType *wgt)
{
	wgt->name = "Mesh Spin";
	wgt->idname = "MESH_WGT_spin";

	wgt->flag = WM_MANIPULATORGROUPTYPE_3D;

	wgt->mmap_params.spaceid = SPACE_VIEW3D;
	wgt->mmap_params.regionid = RGN_TYPE_WINDOW;

	wgt->poll = manipulator_mesh_spin_poll;
	wgt->setup = manipulator_mesh_spin_setup;
	wgt->draw_prepare = manipulator_mesh_spin_draw_prepare;
}

/** \} */

#endif  /* USE_MANIPULATOR */


static int edbm_screw_exec(bContext *C, wmOperator *op)
{
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMesh *bm = em->bm;
	BMEdge *eed;
	BMVert *eve, *v1, *v2;
	BMIter iter, eiter;
	BMOperator spinop;
	float dvec[3], nor[3], cent[3], axis[3], v1_co_global[3], v2_co_global[3];
	int steps, turns;
	int valence;


	turns = RNA_int_get(op->ptr, "turns");
	steps = RNA_int_get(op->ptr, "steps");
	RNA_float_get_array(op->ptr, "center", cent);
	RNA_float_get_array(op->ptr, "axis", axis);

	if (is_zero_v3(axis)) {
		BKE_report(op->reports, RPT_ERROR, "Invalid/unset axis");
		return OPERATOR_CANCELLED;
	}

	/* find two vertices with valence count == 1, more or less is wrong */
	v1 = NULL;
	v2 = NULL;

	BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
		valence = 0;
		BM_ITER_ELEM (eed, &eiter, eve, BM_EDGES_OF_VERT) {
			if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
				valence++;
			}
		}

		if (valence == 1) {
			if (v1 == NULL) {
				v1 = eve;
			}
			else if (v2 == NULL) {
				v2 = eve;
			}
			else {
				v1 = NULL;
				break;
			}
		}
	}

	if (v1 == NULL || v2 == NULL) {
		BKE_report(op->reports, RPT_ERROR, "You have to select a string of connected vertices too");
		return OPERATOR_CANCELLED;
	}

	copy_v3_v3(nor, obedit->obmat[2]);

	/* calculate dvec */
	mul_v3_m4v3(v1_co_global, obedit->obmat, v1->co);
	mul_v3_m4v3(v2_co_global, obedit->obmat, v2->co);
	sub_v3_v3v3(dvec, v1_co_global, v2_co_global);
	mul_v3_fl(dvec, 1.0f / steps);

	if (dot_v3v3(nor, dvec) > 0.0f)
		negate_v3(dvec);

	if (!EDBM_op_init(em, &spinop, op,
	                  "spin geom=%hvef cent=%v axis=%v dvec=%v steps=%i angle=%f space=%m4 use_duplicate=%b",
	                  BM_ELEM_SELECT, cent, axis, dvec, turns * steps, DEG2RADF(360.0f * turns), obedit->obmat, false))
	{
		return OPERATOR_CANCELLED;
	}
	BMO_op_exec(bm, &spinop);
	EDBM_flag_disable_all(em, BM_ELEM_SELECT);
	BMO_slot_buffer_hflag_enable(bm, spinop.slots_out, "geom_last.out", BM_ALL_NOLOOP, BM_ELEM_SELECT, true);
	if (!EDBM_op_finish(em, &spinop, op, true)) {
		return OPERATOR_CANCELLED;
	}

	EDBM_update_generic(em, true, true);

	return OPERATOR_FINISHED;
}

/* get center and axis, in global coords */
static int edbm_screw_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = CTX_wm_view3d(C);
	RegionView3D *rv3d = ED_view3d_context_rv3d(C);

	PropertyRNA *prop;
	prop = RNA_struct_find_property(op->ptr, "center");
	if (!RNA_property_is_set(op->ptr, prop)) {
		RNA_property_float_set_array(op->ptr, prop, ED_view3d_cursor3d_get(scene, v3d));
	}
	if (rv3d) {
		prop = RNA_struct_find_property(op->ptr, "axis");
		if (!RNA_property_is_set(op->ptr, prop)) {
			RNA_property_float_set_array(op->ptr, prop, rv3d->viewinv[1]);
		}
	}

	return edbm_screw_exec(C, op);
}

void MESH_OT_screw(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Screw";
	ot->description = "Extrude selected vertices in screw-shaped rotation around the cursor in indicated viewport";
	ot->idname = "MESH_OT_screw";

	/* api callbacks */
	ot->invoke = edbm_screw_invoke;
	ot->exec = edbm_screw_exec;
	ot->poll = ED_operator_editmesh;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* props */
	RNA_def_int(ot->srna, "steps", 9, 1, 100000, "Steps", "Steps", 3, 256);
	RNA_def_int(ot->srna, "turns", 1, 1, 100000, "Turns", "Turns", 1, 256);

	RNA_def_float_vector(ot->srna, "center", 3, NULL, -1e12f, 1e12f,
	                     "Center", "Center in global view space", -1e4f, 1e4f);
	RNA_def_float_vector(ot->srna, "axis", 3, NULL, -1.0f, 1.0f,
	                     "Axis", "Axis in global view space", -1.0f, 1.0f);
}

/** \} */
