.pragma library

// Evaluates wallpaper property expressions using the established `{ value }` globals.
// Signature: evaluate(properties: object[], overrides: object) -> object.
function evaluate(properties, overrides) {
    const values = ({ });
    const expressions = [];
    for (const property of properties) {
        const key = property.key;
        const value = overrides[key] === undefined ? property.value : overrides[key];
        values[key] = { value: value };
        if (property.condition) expressions.push(property.condition);
        for (const option of property.options || []) {
            if (option.condition) expressions.push(option.condition);
        }
    }

    const conditions = ({ });
    for (const expression of expressions) {
        try {
            const result = Function.apply(null, Object.keys(values).concat("return (" + expression + ");"))
                .apply(null, Object.keys(values).map(key => values[key]));
            conditions[expression] = result === undefined || result === null ? true : Boolean(result);
        } catch (error) {
            conditions[expression] = true;
        }
    }
    return conditions;
}
