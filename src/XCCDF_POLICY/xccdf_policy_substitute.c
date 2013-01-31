/*
 * Copyright 2013 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <libxml/tree.h>

#include "util.h"
#include "xml_iterate.h"
#include "debug_priv.h"
#include "assume.h"
#include "_error.h"
#include "XCCDF/elements.h"
#include "xccdf_policy_priv.h"
#include "public/xccdf_policy.h"

struct _xccdf_text_substitution_data {
	struct xccdf_policy *policy;
	enum {
		_TAILORING_TYPE = 1,
		_DOCUMENT_GENERATION_TYPE = 2,
		_ASSESSMENT_TYPE = 4
	} processing_type;		// Defines behaviour for fix/@use="legacy"
	// TODO: this shall carry also the @xml:lang.
};

static int _xccdf_text_substitution_cb(xmlNode **node, void *user_data)
{
	struct _xccdf_text_substitution_data *data = (struct _xccdf_text_substitution_data *) user_data;
	if (node == NULL || *node == NULL || data == NULL)
		return 1;

	if (oscap_streq((const char *) (*node)->name, "sub") && xccdf_is_supported_namespace((*node)->ns)) {
		if ((*node)->children != NULL)
			dW("The xccdf:sub element SHALL NOT have any content.\n");
		char *sub_idref = (char *) xmlGetProp(*node, BAD_CAST "idref");
		if (oscap_streq(sub_idref, NULL)) {
			oscap_seterr(OSCAP_EFAMILY_XCCDF, "The xccdf:sub MUST have a single @idref attribute.");
			free(sub_idref); // It may be an empty string.
			return 2;
		}
		// Sub element may refer to xccdf:Value or to xccdf:plain-text

		struct xccdf_policy *policy = data->policy;
		assume_ex(policy != NULL, 1);
		struct xccdf_policy_model *model = xccdf_policy_get_model(policy);
		assume_ex(model != NULL, 1);
		struct xccdf_benchmark *benchmark = xccdf_policy_model_get_benchmark(model);
		assume_ex(benchmark != NULL, 1);
		struct xccdf_item *value = xccdf_benchmark_get_item(benchmark, sub_idref);

		const char *result = NULL;
		if (value != NULL && xccdf_item_get_type(value) == XCCDF_VALUE) {
			// When the <xccdf:sub> element's @idref attribute holds the id of an <xccdf:Value>
			// element, the <xccdf:sub> element's @use attribute MUST be consulted.
			const char *sub_use = (const char *) xmlGetProp(*node, BAD_CAST "use");
			if (oscap_streq(sub_use, NULL) || oscap_streq(sub_use, "legacy")) {
				// If the value of the @use attribute is "legacy", then during Tailoring,
				// process the <xccdf:sub> element as if @use was set to "title". but
				// during Document Generation or Assessment, process the <xccdf:sub>
				// element as if @use was set to "value".
				sub_use = strdup((data->processing_type & _TAILORING_TYPE) ? "title" : "value");
			}

			if (oscap_streq(sub_use, "title")) {
				// TODO: @xml:lang
				struct oscap_text_iterator *title_it = xccdf_item_get_title(value);
				if (oscap_text_iterator_has_more(title_it))
					result = oscap_text_get_text(oscap_text_iterator_next(title_it));
				oscap_text_iterator_free(title_it);
			} else {
				if (!oscap_streq(sub_use, "value"))
					dW("xccdf:sub/@idref='%s' has incorrect @use='%s'! Using @use='value' instead.\n", sub_idref, sub_use);
				result = xccdf_policy_get_value_of_item(policy, value);
			}
			oscap_free(sub_use);
		} else { // This xccdf:sub probably refers to the xccdf:plain-text
			result = xccdf_benchmark_get_plain_text(benchmark, sub_idref);
		}

		if (result == NULL) {
			oscap_seterr(OSCAP_EFAMILY_XCCDF, "Could not resolve xccdf:sub/@idref='%s'!", sub_idref);
			oscap_free(sub_idref);
			return 2;
		}
		oscap_free(sub_idref);

		xmlNode *new_node = xmlNewText(BAD_CAST result);
		xmlReplaceNode(*node, new_node);
		xmlFreeNode(*node);
		*node = new_node;
		return 0;
	} else {
		// TODO: <object> elements
		// TODO: <instance> elements
		return 0;
	}
}

char* xccdf_policy_substitute(const char *text, struct xccdf_policy *policy) {
	struct _xccdf_text_substitution_data data;
	data.policy = policy;
	/* We cannot anticipate processing type. But <title>'s are least probable. */
	data.processing_type = _DOCUMENT_GENERATION_TYPE | _ASSESSMENT_TYPE;

	char *resolved_text = NULL;
	if (xml_iterate_dfs(text, &resolved_text, _xccdf_text_substitution_cb, &data) != 0) {
		// Either warning or error occured. Since prototype of this function
		// does not make possible warning notification -> We better scratch that.
		free(resolved_text);
		resolved_text = NULL;
	}
	return resolved_text;
}