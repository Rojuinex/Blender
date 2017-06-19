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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file primitive_manipulator.c
 *  \ingroup wm
 *
 * \name Primitive Manipulator
 *
 * 3D Manipulator
 *
 * \brief Manipulator with primitive drawing type (plane, cube, etc.).
 * Currently only plane primitive supported without own handling, use with operator only.
 */

#include "BIF_gl.h"

#include "BKE_context.h"

#include "BLI_math.h"

#include "DNA_view3d_types.h"

#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_select.h"

#include "MEM_guardedalloc.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_manipulator_library.h"

/* own includes */
#include "manipulator_library_intern.h"

typedef struct PrimitiveManipulator {
	wmManipulator manipulator;
	int style;
} PrimitiveManipulator;


static float verts_plane[4][3] = {
	{-1, -1, 0},
	{ 1, -1, 0},
	{ 1,  1, 0},
	{-1,  1, 0},
};


/* -------------------------------------------------------------------- */

static void manipulator_primitive_draw_geom(
        const float col_inner[4], const float col_outer[4], const int style)
{
	float (*verts)[3];
	uint vert_count = 0;

	if (style == ED_MANIPULATOR_PRIMITIVE_STYLE_PLANE) {
		verts = verts_plane;
		vert_count = ARRAY_SIZE(verts_plane);
	}

	if (vert_count > 0) {
		uint pos = GWN_vertformat_attr_add(immVertexFormat(), "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
		wm_manipulator_vec_draw(col_inner, verts, vert_count, pos, GWN_PRIM_TRI_FAN);
		wm_manipulator_vec_draw(col_outer, verts, vert_count, pos, GWN_PRIM_LINE_LOOP);
		immUnbindProgram();
	}
}

static void manipulator_primitive_draw_intern(
        PrimitiveManipulator *prim, const bool UNUSED(select),
        const bool highlight)
{
	float col_inner[4], col_outer[4];
	float mat[4][4];

	BLI_assert(prim->style != -1);

	manipulator_color_get(&prim->manipulator, highlight, col_outer);
	copy_v4_v4(col_inner, col_outer);
	col_inner[3] *= 0.5f;

	copy_m4_m4(mat, prim->manipulator.matrix);
	mul_mat3_m4_fl(mat, prim->manipulator.scale);

	gpuPushMatrix();
	gpuMultMatrix(mat);

	glEnable(GL_BLEND);
	gpuMultMatrix(prim->manipulator.matrix_offset);
	manipulator_primitive_draw_geom(col_inner, col_outer, prim->style);
	glDisable(GL_BLEND);

	gpuPopMatrix();

	if (prim->manipulator.interaction_data) {
		ManipulatorInteraction *inter = prim->manipulator.interaction_data;

		copy_v4_fl(col_inner, 0.5f);
		copy_v3_fl(col_outer, 0.5f);
		col_outer[3] = 0.8f;

		copy_m4_m4(mat, inter->init_matrix);
		mul_mat3_m4_fl(mat, inter->init_scale);

		gpuPushMatrix();
		gpuMultMatrix(mat);

		glEnable(GL_BLEND);
		gpuMultMatrix(prim->manipulator.matrix_offset);
		manipulator_primitive_draw_geom(col_inner, col_outer, prim->style);
		glDisable(GL_BLEND);

		gpuPopMatrix();
	}
}

static void manipulator_primitive_draw_select(
        const bContext *UNUSED(C), wmManipulator *mpr,
        int selectionbase)
{
	GPU_select_load_id(selectionbase);
	manipulator_primitive_draw_intern((PrimitiveManipulator *)mpr, true, false);
}

static void manipulator_primitive_draw(const bContext *UNUSED(C), wmManipulator *mpr)
{
	manipulator_primitive_draw_intern(
	            (PrimitiveManipulator *)mpr, false,
	            (mpr->state & WM_MANIPULATOR_STATE_HIGHLIGHT));
}

static void manipulator_primitive_setup(wmManipulator *mpr)
{
	PrimitiveManipulator *prim = (PrimitiveManipulator *)mpr;

	prim->manipulator.flag |= WM_MANIPULATOR_DRAW_ACTIVE;
	prim->style = -1;
}

static void manipulator_primitive_invoke(
        bContext *UNUSED(C), wmManipulator *mpr, const wmEvent *UNUSED(event))
{
	ManipulatorInteraction *inter = MEM_callocN(sizeof(ManipulatorInteraction), __func__);

	copy_m4_m4(inter->init_matrix, mpr->matrix);
	inter->init_scale = mpr->scale;

	mpr->interaction_data = inter;
}

/* -------------------------------------------------------------------- */
/** \name Primitive Manipulator API
 *
 * \{ */

#define ASSERT_TYPE_CHECK(mpr) BLI_assert(mpr->type->draw == manipulator_primitive_draw)

void ED_manipulator_primitive3d_set_style(struct wmManipulator *mpr, int style)
{
	ASSERT_TYPE_CHECK(mpr);
	PrimitiveManipulator *prim = (PrimitiveManipulator *)mpr;
	prim->style = style;
}

static void MANIPULATOR_WT_primitive_3d(wmManipulatorType *wt)
{
	/* identifiers */
	wt->idname = "MANIPULATOR_WT_primitive_3d";

	/* api callbacks */
	wt->draw = manipulator_primitive_draw;
	wt->draw_select = manipulator_primitive_draw_select;
	wt->setup = manipulator_primitive_setup;
	wt->invoke = manipulator_primitive_invoke;

	wt->struct_size = sizeof(PrimitiveManipulator);
}

void ED_manipulatortypes_primitive_3d(void)
{
	WM_manipulatortype_append(MANIPULATOR_WT_primitive_3d);
}

/** \} */ // Primitive Manipulator API