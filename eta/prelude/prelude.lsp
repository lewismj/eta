; Eta prelude

; list functions.
(define (nil) '())

; length of list.
(defun (len xs)
  (if (== xs nil) 
    (0) 
    (+ 1 (len (tail xs)))
  )) 

; first element of a list.
(defun (fst xs) ( eval (head xs) ))

; drop n-elements from a list.
(defun (drop n xs)
    (if (== n 0)
        (xs)
        (drop (- n 1) (tail xs))
     ))

; fold a list.
(defun (foldl f z xs)
    (if (== xs nil)
        (z)
        (foldl f (f z (fst xs)) (tail xs))
    ))

; map over the elements of a list.
(defun (map f xs)
    ((if (== xs nil)
        (nil)
        (join (list (f (fst xs))) (map f (tail xs))))
    ))

; filter elements of list.
(defun (filter f xs)
        (if (== xs nil)
            (nil)
            (join (if (f (fst xs)) (head xs) (nil)) (filter f (tail xs)))
        ))
